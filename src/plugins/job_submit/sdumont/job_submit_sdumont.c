/*****************************************************************************\
 *  sdumont.c - Set sdumont in job submit request specifications.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#  if STDC_HEADERS
#    include <string.h>
#  endif
#  if HAVE_SYS_TYPES_H
#    include <sys/types.h>
#  endif /* HAVE_SYS_TYPES_H */
#  if HAVE_UNISTD_H
#    include <unistd.h>
#  endif
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  else /* ! HAVE_INTTYPES_H */
#    if HAVE_STDINT_H
#      include <stdint.h>
#    endif
#  endif /* HAVE_INTTYPES_H */
#else /* ! HAVE_CONFIG_H */
#  include <sys/types.h>
#  include <unistd.h>
#  include <stdint.h>
#  include <string.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <search.h>

#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"
#include "src/common/slurm_accounting_storage.h"

#include "slurm/slurmdb.h"

#define MIN_ACCTG_FREQUENCY 30

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for SLURM authentication) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version   - specifies the version number of the plugin.
 * min_plug_version - specifies the minumum version number of incoming
 *                    messages that this plugin can accept
 */
const char plugin_name[]       	= "Job submit sdumont plugin";
const char plugin_type[]       	= "job_submit/sdumont";
const uint32_t plugin_version   = 100;
const uint32_t min_plug_version = 100;

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

static int sd_item_cmp(void *a, void *b)
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
static float sdumont_part_peso(const char *partition)
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
	slurmdb_job_cond_t *job_cond;
	List jobs;
	ListIterator itr;
	slurmdb_job_rec_t *job;

	/* job conditions */
	job_cond = xmalloc(sizeof(slurmdb_job_cond_t));
	job_cond->without_usage_truncation = 1;
	job_cond->userid_list = NULL;
	job_cond->cluster_list = NULL;
	job_cond->without_steps = 1;
	job_cond->acct_list = list_create(slurm_destroy_char);
	slurm_addto_char_list(job_cond->acct_list, job_desc->account);

	/* particoes e pesos */
	sdumont_pesos_init();
	job_cond->partition_list = list_create(slurm_destroy_char);
	for (i = 0; i > nparts; i++)
		slurm_addto_char_list(job_cond->partition_list, itens[i].part);

	/* start date */
	gettimeofday(&now, NULL);
	timersub(&now, &month, &since);
	job_cond->usage_start = since.tv_sec;

	jobs = slurmdb_jobs_get(db_conn, job_cond);
	if (jobs) {
		itr = list_iterator_create(jobs);
		while((job = list_next(itr)))
			uso += job->tot_cpu_sec * sdumont_part_peso(job->partition);
		list_iterator_destroy(itr);
		list_destroy(jobs);
	}
	list_destroy(job_cond->partition_list);
	list_destroy(job_cond->acct_list);
	xfree(job_cond);
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
	uint64_t uso, cota;
	int ret = SLURM_SUCCESS;

	if (!db_conn)
		ret = SLURM_ERROR;
	else {
		uso = sdumont_uso(db_conn, job_desc);
		cota = sdumont_cota(job_desc->account);

		if (uso > cota) {
			char *new_part;
			/* TODO: notificar administradores */

			info("%s: conta %s ultrapassou a cota", plugin_name, job_desc->account);

			new_part = xstrdup(job_desc->partition);
			xstrcat(new_part, "_low");

			/* existe uma particao equivalente com prioridade menor? */
			if (find_part_record(new_part) != NULL) {
				info("%s: alterando particao da tarefa %u para reduzir prioridade (%s -> %s)",
					plugin_name, job_desc->job_id, job_desc->partition, new_part);

				xfree(job_desc->partition);
				job_desc->partition = new_part;
			} else {
				xfree(new_part);
				error("%s: nao existe particao equivalente de prioridade mais baixa que %s",
					plugin_name, job_desc->partition);
				ret = SLURM_ERROR;
			}
		}
		slurmdb_connection_close(&db_conn);
	}
	slurm_acct_storage_fini();
	return ret;
}

extern int job_modify(struct job_descriptor *job_desc,
					struct job_record *job_ptr, uint32_t submit_uid)
{
	return job_submit(job_desc, submit_uid);
}
