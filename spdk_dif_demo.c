#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"

struct ctrlr_entry {
	struct ctrlr_entry	*next;
	struct spdk_nvme_ctrlr  *ctrlr;
	char 			name[1024];
};

struct ns_entry {
	struct ns_entry		*next;
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
};

static struct ctrlr_entry	*g_ctrlr_list = NULL;
static struct ns_entry		*g_ns_list = NULL;

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(EXIT_FAILURE);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = g_ns_list;
	g_ns_list = entry;

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(EXIT_FAILURE);
	}

	printf("Attached to %s\n", trid->traddr);
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	entry->next = g_ctrlr_list;
	g_ctrlr_list = entry;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	printf("Using controller %s with %d namespaces.\n", entry->name, num_ns);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns != NULL)
			register_ns(ctrlr, ns);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry = g_ns_list;
	struct ctrlr_entry *ctrlr_entry = g_ctrlr_list;

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;
		free(ns_entry);
		ns_entry = next;
	}

	g_ns_list = NULL;

	while (ctrlr_entry) {
		struct ctrlr_entry *next = ctrlr_entry->next;

		spdk_nvme_detach(ctrlr_entry->ctrlr);
		free(ctrlr_entry);
		ctrlr_entry = next;
	}

	g_ctrlr_list = NULL;
}


int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	/* Init env */
	spdk_env_opts_init(&opts);
	opts.name = "spdk_dif_demo";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return EXIT_FAILURE;
	}

	/* Init g_ctrlr_list, g_ns_list */
	printf("Initializing NVMe Controllers\n");

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		cleanup();
		return EXIT_FAILURE;
	}

	if (g_ctrlr_list == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		cleanup();
		return EXIT_FAILURE;
	}

	printf("Initialization complete.\n");

	/* I/O */

	cleanup();
	return 0;
}
