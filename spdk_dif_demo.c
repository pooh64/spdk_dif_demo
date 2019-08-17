#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/dif.h"

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

	uint32_t		block_size;
	uint32_t		md_size;
	uint32_t		io_flags;
	bool			md_interleave;
	bool			pi_loc;
	enum spdk_nvme_pi_type	pi_type;
};

struct io_sequence {
	struct ns_entry		*ns_entry;
	char			*buf;
	bool        		using_cmb_io;
	int			is_completed;

	uint32_t		io_flags;
	struct spdk_dif_ctx	dif_ctx;
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


	entry->block_size	= spdk_nvme_ns_get_extended_sector_size(ns);
	entry->md_size		= spdk_nvme_ns_get_md_size(ns);
	entry->md_interleave	= spdk_nvme_ns_supports_extended_lba(ns);
	entry->pi_loc		= spdk_nvme_ns_get_data(ns)->dps.md_start;
	entry->pi_type		= spdk_nvme_ns_get_pi_type(ns);

	entry->io_flags		= 0;
	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		entry->io_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}
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

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_sequence	*sequence = arg;
	struct spdk_dif_error	dif_error;
	struct iovec		iov;
	int rc;

	sequence->is_completed = 1;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
	}

	iov.iov_base = sequence->buf;
	iov.iov_len  = sequence->ns_entry->block_size - sequence->ns_entry->md_size;
	rc = spdk_dif_verify(&iov, 1, 1, &sequence->dif_ctx, &dif_error);
	if (rc != 0) {
		fprintf(stderr, "I/O DIF verify failed\n");
		fprintf(stderr, "Err type: %d\n", (int)      dif_error.err_type);
		fprintf(stderr, "Actual  : %08" PRIx32 "\n", dif_error.actual);
		fprintf(stderr, "Expected: %08" PRIx32 "\n", dif_error.expected);
		sequence->is_completed = 2;
	}

	printf("I/O Done, no DIF errors detected\n");
	spdk_free(sequence->buf);
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_sequence	*sequence = arg;
	struct ns_entry		*ns_entry = sequence->ns_entry;
	int			rc;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_free_cmb_io_buffer(ns_entry->ctrlr, sequence->buf, 0x1000);
	} else {
		spdk_free(sequence->buf);
	}
	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	rc = spdk_nvme_ns_cmd_write_with_md(ns_entry->ns, ns_entry->qpair,
					    sequence->buf, NULL, 0, 1,
					    read_complete, &sequence,
					    ns_entry->io_flags, 0xffff, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}


static void
memrand(char *buf, size_t size)
{
	srand(time(NULL));
	for (; size; buf++, size--)
		*buf = (char) rand();
}


static void
demo(void)
{
	struct ns_entry		*ns_entry;
	struct io_sequence	sequence;
	struct iovec		iov;
	int			rc;

	ns_entry = g_ns_list;
	while (ns_entry != NULL) {
		if (!(ns_entry->io_flags | SPDK_NVME_IO_FLAGS_PRCHK_GUARD) ||
		    !ns_entry->md_interleave				   ||
		    ns_entry->md_size == 0) {
			printf("INFO: Skip no-guard NS\n");
			ns_entry = ns_entry->next;
			continue;
		}

		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return;
		}

		sequence.using_cmb_io = 1;
		sequence.buf = spdk_nvme_ctrlr_alloc_cmb_io_buffer(ns_entry->ctrlr, 0x1000);
		if (sequence.buf == NULL) {
			sequence.using_cmb_io = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		}
		if (sequence.buf == NULL) {
			printf("ERROR: write buffer allocation failed\n");
			return;
		}
		if (sequence.using_cmb_io) {
			printf("INFO: using controller memory buffer for IO\n");
		} else {
			printf("INFO: using host memory buffer for IO\n");
		}
		sequence.is_completed = 0;
		sequence.ns_entry = ns_entry;


		memrand(sequence.buf, ns_entry->block_size);

		rc = spdk_dif_ctx_init(&sequence.dif_ctx, ns_entry->block_size,
				       ns_entry->md_size, ns_entry->md_interleave,
				       ns_entry->pi_loc,
				       (enum spdk_dif_type) ns_entry->pi_type,
				       ns_entry->io_flags, 0, 0xffff, 0, 0, 0);
		if (rc != 0) {
			fprintf(stderr, "Initialization of DIF context failed\n");
			exit(1);
		}

		iov.iov_base = sequence.buf;
		iov.iov_len  = ns_entry->block_size - ns_entry->md_size;
		rc = spdk_dif_generate(&iov, 1, 1, &sequence.dif_ctx);
		if (rc != 0) {
			fprintf(stderr, "Generation of DIF failed\n");
			exit(1);
		}

		rc = spdk_nvme_ns_cmd_write_with_md(ns_entry->ns, ns_entry->qpair,
						    sequence.buf, NULL, 0, 1,
						    write_complete, &sequence,
						    ns_entry->io_flags, 0xffff, 0);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}

		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
		ns_entry = ns_entry->next;
	}
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

	demo();

	cleanup();
	return EXIT_SUCCESS;
}
