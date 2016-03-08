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

#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"

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

static int is_account_overused(struct job_descriptor *job_desc)
{
	uint32_t uso = 0, cota = 0;
	struct timeval now;
	struct timeval month = {30 * 24 * 60 * 60, 0};
	struct timeval since;

	gettimeofday(&now, NULL);
	timesub(&now, &month, &since);

	/* TODO1: consultar uso do usario nos ultimos 30 dias */
	/* TODO2: consultar cota do usuario */

	return uso > cota;
}

extern int job_submit(struct job_descriptor *job_desc, uint32_t submit_uid)
{
	if (is_account_overused(job_desc)) {
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

			return SLURM_SUCCESS;
		} else {
			xfree(new_part);
			error("%s: nao existe particao equivalente de prioridade mais baixa que %s",
				plugin_name, job_desc->partition);
			return SLURM_ERROR;
		}
	}
	return SLURM_SUCCESS;
}

extern int job_modify(struct job_descriptor *job_desc,
					struct job_record *job_ptr, uint32_t submit_uid)
{
	return job_submit(job_desc, submit_uid);
}
