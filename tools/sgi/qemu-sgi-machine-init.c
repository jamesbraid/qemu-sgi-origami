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
#include "hw/mips/sgi/initial-flash.h"
#include "hw/mips/sgi/prom-image.h"
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

/*
 * The IP27 configuration-block values the machine patches into node
 * flash built for @t: the product values from the topology's
 * ip27-firmware record, which the catalogue compiler requires on every
 * SN0 topology, and the processor values from its first node, as the
 * machine takes them.
 */
static SGIIP27FlashValues ip27_flash_values(const SGIModelTopology *t)
{
    const SGIModelProcessor *cpu = t->nodes->value->cpu_slots->value->assembly;
    const SGIModelIp27FirmwareConfig *firmware = t->ip27_firmware;

    g_assert(firmware);
    return (SGIIP27FlashValues) {
        .machine_type = firmware->machine_type,
        .read_cycles = firmware->flash_read_cycles,
        .write_cycles = firmware->flash_write_cycles,
        .cpu_mode_bits = cpu->mode_bits,
        .cpu_clock_hz = cpu->clock_hz,
    };
}

/*
 * The decoded image for @input, sized for @item. The boot PROM fills a
 * whole IP27 flash, half an IP35 flash (one of its two positions) or
 * the Octane flash as a raw image. The I/O PROM is stored as supplied,
 * container included, so the whole file must fit.
 */
static SGIPromImage *load_input(const SGIModelInitInput *input,
                                const char *path,
                                const SGIModelStorageItem *item, Error **errp)
{
    g_autoptr(SGIPromImage) image = NULL;
    g_autoptr(GError) gerr = NULL;
    g_autofree char *contents = NULL;
    gsize length;

    switch (input->kind) {
    case SGI_MODEL_INIT_INPUT_KIND_IP27_PROM:
        return sgi_prom_image_load(path, item->size, errp);
    case SGI_MODEL_INIT_INPUT_KIND_IP35_PROM:
        return sgi_prom_image_load(path, item->size / SGI_IP35_FLASH_POSITIONS,
                                   errp);
    case SGI_MODEL_INIT_INPUT_KIND_IP30_PROM:
        /* The Octane machine copies its -bios file without decoding it. */
        if (!g_file_get_contents(path, &contents, &length, &gerr)) {
            error_setg(errp, "cannot read PROM image '%s': %s", path,
                       gerr->message);
            return NULL;
        }
        if (!length || length > item->size) {
            error_setg(errp, "PROM image '%s' must hold 1 to %" PRIu64
                       " bytes", path, item->size);
            return NULL;
        }
        image = g_new0(SGIPromImage, 1);
        image->file = g_bytes_new_take(g_steal_pointer(&contents), length);
        image->payload = g_bytes_ref(image->file);
        return g_steal_pointer(&image);
    case SGI_MODEL_INIT_INPUT_KIND_IO6_PROM:
        image = sgi_prom_image_load(path, item->size, errp);
        if (image && g_bytes_get_size(image->file) > item->size) {
            error_setg(errp, "I/O PROM image '%s' exceeds flash capacity "
                       "(%" PRIu64 " bytes)", path, item->size);
            return NULL;
        }
        return g_steal_pointer(&image);
    default:
        g_assert_not_reached();
    }
}

/* Fill @buf with @item's initial contents. */
static void build_item(const SGIModelStorageItem *item,
                       const SGIModelInitInput *input,
                       const SGIPromImage *image, const SGIModelTopology *t,
                       uint8_t *buf)
{
    const uint8_t *data;
    size_t length;
    SGIIP27FlashValues values;

    switch (item->initial) {
    case SGI_MODEL_STORAGE_INITIAL_ZERO:
        memset(buf, 0, item->size);
        return;
    case SGI_MODEL_STORAGE_INITIAL_IO_PROM:
        data = g_bytes_get_data(image->file, &length);
        sgi_io6_initial_flash(buf, item->size, data, length);
        return;
    case SGI_MODEL_STORAGE_INITIAL_BOOT_PROM:
        break;
    default:
        g_assert_not_reached();
    }

    data = g_bytes_get_data(image->payload, &length);
    switch (input->kind) {
    case SGI_MODEL_INIT_INPUT_KIND_IP27_PROM:
        values = ip27_flash_values(t);
        sgi_ip27_initial_flash(buf, item->size, &values, data, length);
        break;
    case SGI_MODEL_INIT_INPUT_KIND_IP35_PROM:
        sgi_ip35_initial_flash(buf, item->size, data, length);
        break;
    case SGI_MODEL_INIT_INPUT_KIND_IP30_PROM:
        memset(buf, 0xff, item->size);
        memcpy(buf, data, length);
        break;
    default:
        g_assert_not_reached();
    }
}

/* Write @size bytes to the new file @name in @dirfd and flush them. */
static bool write_store(int dirfd, const char *name, const uint8_t *buf,
                        size_t size, Error **errp)
{
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    0666);

    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot create '%s'", name);
        return false;
    }
    if (qemu_write_full(fd, buf, size) != size) {
        error_setg_errno(errp, errno, "cannot write '%s'", name);
        close(fd);
        return false;
    }
    if (fsync(fd) < 0) {
        error_setg_errno(errp, errno, "cannot flush '%s'", name);
        close(fd);
        return false;
    }
    if (close(fd) < 0) {
        error_setg_errno(errp, errno, "cannot close '%s'", name);
        return false;
    }
    return true;
}

static bool sync_directory(const char *path, Error **errp)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int ret;

    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot open '%s'", path);
        return false;
    }
    ret = fsync(fd);
    close(fd);
    if (ret < 0) {
        error_setg_errno(errp, errno, "cannot flush '%s'", path);
        return false;
    }
    return true;
}

/*
 * Move the finished @tmp to @dir without replacing anything there. Where
 * the kernel and filesystem can say so atomically they do; elsewhere the
 * caller's earlier check that @dir does not exist is all there is.
 */
static bool publish_directory(const char *tmp, const char *dir, Error **errp)
{
#ifdef RENAME_NOREPLACE
    if (renameat2(AT_FDCWD, tmp, AT_FDCWD, dir, RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno != EINVAL && errno != ENOSYS) {
        error_setg_errno(errp, errno, "cannot create '%s'", dir);
        return false;
    }
#endif
    if (access(dir, F_OK) == 0 || errno != ENOENT) {
        error_setg(errp, "'%s' already exists", dir);
        return false;
    }
    if (rename(tmp, dir) < 0) {
        error_setg_errno(errp, errno, "cannot create '%s'", dir);
        return false;
    }
    return true;
}

/* Refuse anything already at @dir, a dangling symbolic link included. */
static bool check_absent(const char *dir, Error **errp)
{
    struct stat st;

    if (lstat(dir, &st) == 0) {
        error_setg(errp, "'%s' already exists", dir);
        return false;
    }
    if (errno != ENOENT) {
        error_setg_errno(errp, errno, "cannot check '%s'", dir);
        return false;
    }
    return true;
}

/*
 * Build every storage item @offering lists into a private directory
 * beside @dir, then rename it to @dir, so a failure leaves nothing at
 * @dir and nothing beside it.
 */
static bool create_machine(const SGIModelOffering *offering,
                           const InitInputs *inputs, const char *dir,
                           Error **errp)
{
    g_autoptr(GHashTable) images = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL,
        (GDestroyNotify)sgi_prom_image_free);
    g_autofree char *parent = NULL;
    g_autofree char *base = NULL;
    g_autofree char *tmp = NULL;
    const SGIModelStorageItemList *entry;
    const SGIModelTopology *chassis, *t;
    bool published = false, ok = false;
    int dirfd;
    Error *sync_err = NULL;

    chassis = sgi_model_product_topology(sgi_machines(), offering->product,
                                         offering->topology);
    t = chassis ? sgi_model_resolve_population(chassis, offering->nodes, errp)
                : NULL;
    if (!t) {
        if (!chassis) {
            error_setg(errp, "%s has no topology '%s'", offering->product,
                       offering->topology);
        }
        return false;
    }
    if (!check_absent(dir, errp)) {
        return false;
    }
    parent = g_path_get_dirname(dir);
    base = g_path_get_basename(dir);

    /* Read every image first: a bad one should not get as far as a file. */
    for (entry = offering->storage; entry; entry = entry->next) {
        const SGIModelStorageItem *item = entry->value;
        const char *name = SGIModelStorageInitial_str(item->initial);
        const SGIModelInitInput *input;
        SGIPromImage *image;

        g_assert(!strchr(item->name, '/'));
        if (item->initial == SGI_MODEL_STORAGE_INITIAL_ZERO ||
            g_hash_table_contains(images, name)) {
            continue;
        }
        input = find_input(offering, name);
        if (!input) {
            error_setg(errp, "%s builds %s from '%s', which is not one of "
                       "its init inputs", offering->machine_options,
                       item->name, name);
            return false;
        }
        image = load_input(input, input_path(inputs, name), item, errp);
        if (!image) {
            return false;
        }
        g_hash_table_insert(images, (gpointer)name, image);
        /*
         * A Bedrock machine takes only a payload of exactly its node's
         * PROM size as -bios; build no flash it could not have built.
         */
        if (input->kind == SGI_MODEL_INIT_INPUT_KIND_IP35_PROM &&
            g_bytes_get_size(image->payload) != t->nodes->value->flash->size) {
            error_setg(errp, "boot PROM '%s' decodes to %zu bytes; %s needs "
                       "exactly %" PRIu64, input_path(inputs, name),
                       g_bytes_get_size(image->payload),
                       offering->machine_options,
                       t->nodes->value->flash->size);
            return false;
        }
    }

    tmp = g_strdup_printf("%s/.%s.XXXXXX", parent, base);
    if (!g_mkdtemp(tmp)) {
        error_setg_errno(errp, errno, "cannot create a directory beside "
                         "'%s'", dir);
        return false;
    }
    dirfd = open(tmp, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        error_setg_errno(errp, errno, "cannot open '%s'", tmp);
        rmdir(tmp);
        return false;
    }

    for (entry = offering->storage; entry; entry = entry->next) {
        const SGIModelStorageItem *item = entry->value;
        const char *initial = SGIModelStorageInitial_str(item->initial);
        g_autofree uint8_t *buf = g_malloc(item->size);
        g_autofree char *file = g_strdup_printf("%s.raw", item->name);

        build_item(item, find_input(offering, initial),
                   g_hash_table_lookup(images, initial), t, buf);
        if (!write_store(dirfd, file, buf, item->size, errp)) {
            error_prepend(errp, "%s: ", tmp);
            goto out;
        }
    }
    if (!sync_directory(tmp, errp) || !publish_directory(tmp, dir, errp)) {
        goto out;
    }
    published = true;
    ok = true;
    /*
     * DIR is complete and in place. A failed flush of its parent only
     * weakens how durable the rename is, and a retry would find DIR
     * already there, so it is reported but does not fail the run.
     */
    if (!sync_directory(parent, &sync_err)) {
        error_prepend(&sync_err, "created '%s', but ", dir);
        warn_report_err(sync_err);
    }

out:
    if (!published) {
        /* Take back every file this run created, and the directory. */
        for (entry = offering->storage; entry; entry = entry->next) {
            g_autofree char *file =
                g_strdup_printf("%s.raw", entry->value->name);

            unlinkat(dirfd, file, 0);
        }
        rmdir(tmp);
    }
    close(dirfd);
    return ok;
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
    g_autofree char *dir = NULL;
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

    dir = g_strdup(argv[optind]);
    /* "new/" names the directory new, not a place inside it. */
    while (strlen(dir) > 1 && g_str_has_suffix(dir, "/")) {
        dir[strlen(dir) - 1] = '\0';
    }

    offering = resolve_machine(machine, &err);
    if (!offering || !check_inputs(offering, &inputs, &err) ||
        !create_machine(offering, &inputs, dir, &err)) {
        error_report_err(err);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
