/*****************************************************************************\
 *  sdumont.c - Set sdumont in job submit request specifications.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <search.h>

#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"
#include "src/common/slurm_accounting_storage.h"

#include "slurm/slurmdb.h"

const char plugin_name[]       	= "Job submit sdumont plugin";
const char plugin_type[]       	= "job_submit/sdumont";
#if (SLURM_VERSION_NUMBER >= SLURM_VERSION_NUM(15,8,0))
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;
#else
const uint32_t plugin_version   = 100;
const uint32_t min_plug_version = 100;
#endif

/* retorna uma conexao com o banco de dados de contabilidade de tarefas */
static void * sdumont_db_conn()
{
       char *acct_type;
       void *db_conn = NULL;

       slurm_acct_storage_init(NULL);
       acct_type = slurm_get_accounting_storage_type();
       if (strcasecmp(acct_type, "accounting_storage/slurmdbd") &&
                       strcasecmp(acct_type, "accounting_storage/mysql")) {
               error("%s: plugin de contabilidade de tarefas nao suportado: %s",
                       plugin_name, acct_type);
               goto end;
       }

       db_conn = slurmdb_connection_get();
       if (errno) {
               error("%s: nao foi possivel estabelecer conexao com banco de dados",
                               plugin_name);
               db_conn = NULL;
       }

end:
       xfree(acct_type);
       return db_conn;
}

size_t nparts = 0, capacidade = 5;
struct sd_item {
	char *part;
	float peso;
} *itens;

static int sd_item_cmp(const void *a, const void *b)
{
	return strcmp(((struct sd_item *) a)->part, ((struct sd_item *) b)->part);
}

/* carrega a relacao entre particoes e pesos */
static void sdumont_pesos_init()
{
	struct sd_item um;
	FILE *arq;
	char *linha = NULL;
	size_t taml = 0;
	itens = xmalloc(capacidade * sizeof(struct sd_item));
	arq = fopen("/etc/slurm/sdumont-particao-peso.conf", "r");
	if (arq != NULL) {
		while (getline(&linha, &taml, arq) != -1) {
			if (nparts >= capacidade) {
				capacidade *= 2;
				itens = xrealloc(itens, capacidade * sizeof(struct sd_item));
			}
			if (sscanf(linha, "%ms,%f", &um.part, &um.peso) == 2) {
				if (!lsearch(&um, itens, &nparts, sizeof(struct sd_item), sd_item_cmp))
					error("%s: erro no lsearch", plugin_name);
			}
		}
		free(linha);
		fclose(arq);
	}
}

/* retorna o peso de uma particao */
static float sdumont_part_peso(char *partition)
{
	struct sd_item i;
	i.part = partition;
	return ((struct sd_item *) lfind(&i, itens, &nparts, sizeof(struct sd_item), sd_item_cmp))->peso;
}

static void sdumont_pesos_fini()
{
	while (nparts)
		free(itens[--nparts].part);
	xfree(itens);
}

/* retorna o uso que a conta fez do cluster nos ultimos 30 dias */
static uint64_t sdumont_uso(void *db_conn, struct job_descriptor *job_desc)
{
	uint64_t uso = 0;
	int i;

	/* start date */
	struct timeval now;
	struct timeval month = {30 * 24 * 60 * 60, 0};
	struct timeval since;

	/* job conditions */
	slurmdb_job_cond_t job_cond;
	List jobs;
	ListIterator itr;
	slurmdb_job_rec_t *job;

	/* add job conditions - begin */
	bzero(&job_cond, sizeof(slurmdb_job_cond_t));
	job_cond.acct_list = list_create(slurm_destroy_char);
	slurm_addto_char_list(job_cond.acct_list, job_desc->account);

	job_cond.associd_list = list_create(slurm_destroy_char);
	job_cond.cluster_list = list_create(slurm_destroy_char);
	job_cond.cpus_max = UINT32_MAX;
	job_cond.cpus_min = 1;
	job_cond.duplicates = 0;
	job_cond.exitcode = 0;
	job_cond.groupid_list = list_create(slurm_destroy_char);
	job_cond.jobname_list = list_create(slurm_destroy_char);
	job_cond.nodes_max = UINT32_MAX;
	job_cond.nodes_min = 1;

	/* particoes e pesos */
	sdumont_pesos_init();
	job_cond.partition_list = list_create(slurm_destroy_char);
	for (i = 0; i > nparts; i++)
		slurm_addto_char_list(job_cond.partition_list, itens[i].part);

	job_cond.qos_list = list_create(slurm_destroy_char);
	job_cond.resv_list = list_create(slurm_destroy_char);
	job_cond.resvid_list = list_create(slurm_destroy_char);
	job_cond.state_list = list_create(slurm_destroy_char);
	job_cond.step_list = list_create(slurm_destroy_char);
	job_cond.timelimit_max = UINT32_MAX;
	job_cond.timelimit_min = 0;

	job_cond.usage_end = (time_t) -1;
	/* start date */
	gettimeofday(&now, NULL);
	timersub(&now, &month, &since);
	job_cond.usage_start = since.tv_sec;

	job_cond.used_nodes = NULL;

	job_cond.userid_list = list_create(slurm_destroy_char);
	job_cond.wckey_list = list_create(slurm_destroy_char);

	job_cond.without_steps = 1;
	job_cond.without_usage_truncation = 1;
	/* add job conditions - end */

	jobs = slurmdb_jobs_get(db_conn, &job_cond);
	if (jobs) {
		itr = list_iterator_create(jobs);
		while((job = list_next(itr)))
			uso += job->tot_cpu_sec * sdumont_part_peso(job->partition);
		list_iterator_destroy(itr);
		list_destroy(jobs);
	}

	/* free job conditions - begin */
	slurmdb_destroy_job_cond(&job_cond);
	/* free job conditions - end */
	sdumont_pesos_fini();
	return uso;
}

/* retorna a cota de uma conta */
static uint64_t sdumont_cota(const char *account)
{
	uint64_t cota = 0, lido;
	char *padrao;

	FILE *arq;
	char *linha = NULL;
	size_t taml = 0;
	arq = fopen("/etc/slurm/sdumont-conta-cota.conf", "r");
	if (arq != NULL) {
		padrao = malloc(strlen(account) + 5);
		sprintf(padrao, "%s,%%lu", account);
		while (getline(&linha, &taml, arq) != -1) {
			if (sscanf(linha, padrao, &lido) == 1) {
				cota = lido;
				break;
			}
		}
		free(linha);
		free(padrao);
		fclose(arq);
	}
	return cota;
}

extern int job_submit(struct job_descriptor *job_desc, uint32_t submit_uid)
{
	void *db_conn = sdumont_db_conn();
	uint64_t cota, uso;
	int ret = SLURM_SUCCESS;
	job_info_msg_t *job_buffer_ptr = NULL;

	cota = sdumont_cota(job_desc->account);
	uso = sdumont_uso(db_conn, job_desc);
	if (uso > cota) {
		/* TODO: notificar administradores */
		info("%s: conta %s ultrapassou a cota. diminuindo prioridade", plugin_name, job_desc->account);
		job_desc->priority /= 2;
	}

	return ret;
}

extern int job_modify(struct job_descriptor *job_desc,
					struct job_record *job_ptr, uint32_t submit_uid)
{
	return job_submit(job_desc, submit_uid);
}
