/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * qemu-sgi-machine-init: create a new SGI machine's writable state.
 *
 * The compiled machine catalogue lists, for every -M selection, the
 * writable stores the machine attaches and the images their initial
 * contents are built from. This tool resolves a selection with the same
 * code the machine uses and builds those contents with the same code the
 * machine uses when it has no file, so a machine started on the result
 * sees what an unbacked one would.
 *
 * Copyright (c) 2026 James Braid
 */

#include "qemu/osdep.h"
#include <getopt.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/help-texts.h"
#include "qemu-version.h"
#include "hw/mips/sgi/sgi-machines.h"

#define CMD_NAME "qemu-sgi-machine-init"

/* The images a new machine's storage reads, by their init-inputs name. */
typedef struct InitInputs {
    const char *boot_prom;
    const char *io_prom;
} InitInputs;

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: " CMD_NAME " --machine OPTIONS [--boot-prom FILE] "
            "[--io-prom FILE] DIR\n"
            "       " CMD_NAME " --machine help\n"
            "\n"
            "Create the directory DIR holding every writable store the SGI\n"
            "machine selected by OPTIONS attaches, one DIR/<name>.raw file\n"
            "per store, with the contents a new machine starts from.\n"
            "OPTIONS is one of the -M values '--machine help' lists.\n"
            "\n"
            "  -M, --machine OPTIONS  the machine selection, or 'help'\n"
            "  -b, --boot-prom FILE   the node boot PROM image\n"
            "  -i, --io-prom FILE     the BASEIO or GIGAchannel I/O PROM image\n"
            "  -h, --help             print this text\n"
            "  -V, --version          print the version\n");
}

static void usage_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void usage_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
    error_printf("Try '" CMD_NAME " --help' for more information.\n");
    exit(EXIT_FAILURE);
}

/* Every selection the catalogue lists, once each. */
static void list_machines(void)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    const SGIModelOfferingList *entry;

    for (entry = sgi_machines()->offerings; entry; entry = entry->next) {
        const SGIModelOffering *offering = entry->value;

        if (g_hash_table_add(seen, offering->machine_options)) {
            printf("%-56s %s\n", offering->machine_options,
                   offering->display_name);
        }
    }
}

/*
 * The offering @options selects. It is resolved as the machine resolves
 * -M, then held to the catalogue's own spelling of it: a launcher stores
 * that string beside the files, so one machine has one name.
 */
static const SGIModelOffering *resolve_machine(const char *options,
                                               Error **errp)
{
    const SGIModelOffering *offering =
        sgi_model_resolve_options(sgi_machines(), options, errp);

    if (!offering) {
        return NULL;
    }
    if (!g_str_equal(options, offering->machine_options)) {
        error_setg(errp, "'%s' is not a listed machine", options);
        error_append_hint(errp, "It selects '%s'; '--machine help' lists "
                          "every machine.\n", offering->machine_options);
        return NULL;
    }
    return offering;
}

/* The file supplied for the init input @name, or NULL. */
static const char *input_path(const InitInputs *inputs, const char *name)
{
    if (g_str_equal(name, "boot-prom")) {
        return inputs->boot_prom;
    }
    if (g_str_equal(name, "io-prom")) {
        return inputs->io_prom;
    }
    return NULL;
}

static const SGIModelInitInput *find_input(const SGIModelOffering *offering,
                                           const char *name)
{
    const SGIModelInitInputList *entry;

    for (entry = offering->init_inputs; entry; entry = entry->next) {
        if (g_str_equal(entry->value->name, name)) {
            return entry->value;
        }
    }
    return NULL;
}

/*
 * Refuse an image the machine does not read and a missing required one,
 * before anything is created.
 */
static bool check_inputs(const SGIModelOffering *offering,
                         const InitInputs *inputs, Error **errp)
{
    static const char *const names[] = { "boot-prom", "io-prom" };
    const SGIModelInitInputList *entry;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(names); i++) {
        if (input_path(inputs, names[i]) && !find_input(offering, names[i])) {
            error_setg(errp, "%s takes no --%s", offering->machine_options,
                       names[i]);
            return false;
        }
    }
    for (entry = offering->init_inputs; entry; entry = entry->next) {
        const SGIModelInitInput *input = entry->value;
        bool known = false;

        for (i = 0; i < ARRAY_SIZE(names); i++) {
            known |= g_str_equal(input->name, names[i]);
        }
        if (!known) {
            error_setg(errp, "%s needs init input '%s', which this tool "
                       "does not know", offering->machine_options,
                       input->name);
            return false;
        }
        if (input->required && !input_path(inputs, input->name)) {
            error_setg(errp, "%s requires --%s", offering->machine_options,
                       input->name);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        { "machine", required_argument, NULL, 'M' },
        { "boot-prom", required_argument, NULL, 'b' },
        { "io-prom", required_argument, NULL, 'i' },
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'V' },
        { NULL, 0, NULL, 0 },
    };
    InitInputs inputs = { 0 };
    const SGIModelOffering *offering;
    const char *machine = NULL;
    Error *err = NULL;
    int c;

    error_init(argv[0]);
    while ((c = getopt_long(argc, argv, "+M:b:i:hV", long_options,
                            NULL)) != -1) {
        switch (c) {
        case 'M':
            machine = optarg;
            break;
        case 'b':
            inputs.boot_prom = optarg;
            break;
        case 'i':
            inputs.io_prom = optarg;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        case 'V':
            printf(CMD_NAME " version " QEMU_FULL_VERSION "\n"
                   QEMU_COPYRIGHT "\n");
            return EXIT_SUCCESS;
        default:
            error_printf("Try '" CMD_NAME " --help' for more "
                         "information.\n");
            return EXIT_FAILURE;
        }
    }

    if (!machine) {
        usage_error("--machine is required");
    }
    if (g_str_equal(machine, "help")) {
        if (optind != argc || inputs.boot_prom || inputs.io_prom) {
            usage_error("--machine help takes no other arguments");
        }
        list_machines();
        return EXIT_SUCCESS;
    }
    if (optind == argc) {
        usage_error("no directory named");
    }
    if (argc - optind > 1) {
        usage_error("unexpected argument '%s'", argv[optind + 1]);
    }

    offering = resolve_machine(machine, &err);
    if (!offering || !check_inputs(offering, &inputs, &err)) {
        error_report_err(err);
        return EXIT_FAILURE;
    }
    error_report("%s: creating storage is not implemented yet",
                 offering->machine_options);
    return EXIT_FAILURE;
}
