/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HW_MISC_SGI_DIAGNOSTIC_H
#define HW_MISC_SGI_DIAGNOSTIC_H

typedef void DeviceDiagnosticRecord(void *opaque, const char *component,
                                    const char *operation, uint64_t address,
                                    uint64_t value, unsigned int size,
                                    bool has_before_after, uint64_t before,
                                    uint64_t after);

typedef struct DeviceDiagnosticSink {
    DeviceDiagnosticRecord *record;
    void *opaque;
} DeviceDiagnosticSink;

static inline void device_diagnostic_record(DeviceDiagnosticSink *sink,
                                            const char *component,
                                            const char *operation,
                                            uint64_t address, uint64_t value,
                                            unsigned int size,
                                            bool has_before_after,
                                            uint64_t before, uint64_t after)
{
    if (sink && sink->record) {
        sink->record(sink->opaque, component, operation, address, value,
                     size, has_before_after, before, after);
    }
}

#endif /* HW_MISC_SGI_DIAGNOSTIC_H */
