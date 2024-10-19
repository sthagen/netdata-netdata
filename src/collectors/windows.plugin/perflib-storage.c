// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows_plugin.h"
#include "windows-internals.h"

#define _COMMON_PLUGIN_NAME PLUGIN_WINDOWS_NAME
#define _COMMON_PLUGIN_MODULE_NAME "PerflibStorage"
#include "../common-contexts/common-contexts.h"

struct logical_disk {
    usec_t last_collected;
    bool collected_metadata;

    STRING *filesystem;

    RRDSET *st_disk_space;
    RRDDIM *rd_disk_space_used;
    RRDDIM *rd_disk_space_free;

    COUNTER_DATA percentDiskFree;
    // COUNTER_DATA freeMegabytes;
};

struct physical_disk {
    usec_t last_collected;
    bool collected_metadata;

    STRING *device;
    STRING *mount_point;

    ND_DISK_IO disk_io;
    COUNTER_DATA diskReadBytesPerSec;
    COUNTER_DATA diskWriteBytesPerSec;

    ND_DISK_OPS disk_ops;
    COUNTER_DATA diskReadsPerSec;
    COUNTER_DATA diskWritesPerSec;

    ND_DISK_UTIL disk_util;
    COUNTER_DATA percentIdleTime;

    ND_DISK_BUSY disk_busy;
    COUNTER_DATA percentDiskTime;

    ND_DISK_IOTIME disk_iotime;
    COUNTER_DATA percentDiskReadTime;
    COUNTER_DATA percentDiskWriteTime;

    ND_DISK_QOPS disk_qops;
    COUNTER_DATA currentDiskQueueLength;

    COUNTER_DATA averageDiskQueueLength;
    COUNTER_DATA averageDiskReadQueueLength;
    COUNTER_DATA averageDiskWriteQueueLength;
    COUNTER_DATA averageDiskSecondsPerTransfer;
    COUNTER_DATA averageDiskSecondsPerRead;
    COUNTER_DATA averageDiskSecondsPerWrite;
    COUNTER_DATA diskTransfersPerSec;
    COUNTER_DATA diskBytesPerSec;
    COUNTER_DATA averageDiskBytesPerTransfer;
    COUNTER_DATA averageDiskBytesPerRead;
    COUNTER_DATA averageDiskBytesPerWrite;
    COUNTER_DATA splitIoPerSec;
};

struct physical_disk system_physical_total = {
    .collected_metadata = true,
};

static void dict_logical_disk_insert_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused) {
    struct logical_disk *d = value;

    d->percentDiskFree.key = "% Free Space";
    // d->freeMegabytes.key = "Free Megabytes";
}

static void logical_disk_cleanup(struct logical_disk *d) {
    rrdset_is_obsolete___safe_from_collector_thread(d->st_disk_space);
}

static void physical_disk_initialize(struct physical_disk *d) {
    d->percentIdleTime.key = "% Idle Time";
    d->percentDiskTime.key = "% Disk Time";
    d->percentDiskReadTime.key = "% Disk Read Time";
    d->percentDiskWriteTime.key = "% Disk Write Time";
    d->currentDiskQueueLength.key = "Current Disk Queue Length";
    d->averageDiskQueueLength.key = "Avg. Disk Queue Length";
    d->averageDiskReadQueueLength.key = "Avg. Disk Read Queue Length";
    d->averageDiskWriteQueueLength.key = "Avg. Disk Write Queue Length";
    d->averageDiskSecondsPerTransfer.key = "Avg. Disk sec/Transfer";
    d->averageDiskSecondsPerRead.key = "Avg. Disk sec/Read";
    d->averageDiskSecondsPerWrite.key = "Avg. Disk sec/Write";
    d->diskTransfersPerSec.key = "Disk Transfers/sec";
    d->diskReadsPerSec.key = "Disk Reads/sec";
    d->diskWritesPerSec.key = "Disk Writes/sec";
    d->diskBytesPerSec.key = "Disk Bytes/sec";
    d->diskReadBytesPerSec.key = "Disk Read Bytes/sec";
    d->diskWriteBytesPerSec.key = "Disk Write Bytes/sec";
    d->averageDiskBytesPerTransfer.key = "Avg. Disk Bytes/Transfer";
    d->averageDiskBytesPerRead.key = "Avg. Disk Bytes/Read";
    d->averageDiskBytesPerWrite.key = "Avg. Disk Bytes/Write";
    d->splitIoPerSec.key = "Split IO/Sec";
}

static void physical_disk_cleanup(struct physical_disk *d) {
    string_freez(d->device);
    string_freez(d->mount_point);

    rrdset_is_obsolete___safe_from_collector_thread(d->disk_io.st_io);
    rrdset_is_obsolete___safe_from_collector_thread(d->disk_ops.st_ops);
    rrdset_is_obsolete___safe_from_collector_thread(d->disk_util.st_util);
    rrdset_is_obsolete___safe_from_collector_thread(d->disk_busy.st_busy);
    rrdset_is_obsolete___safe_from_collector_thread(d->disk_iotime.st_iotime);
    rrdset_is_obsolete___safe_from_collector_thread(d->disk_qops.st_qops);
}

void dict_physical_disk_insert_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused) {
    struct physical_disk *pd = value;
    physical_disk_initialize(pd);
}

static DICTIONARY *logicalDisks = NULL, *physicalDisks = NULL;
static void initialize(void) {
    physical_disk_initialize(&system_physical_total);

    logicalDisks = dictionary_create_advanced(DICT_OPTION_DONT_OVERWRITE_VALUE |
                                                  DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct logical_disk));

    dictionary_register_insert_callback(logicalDisks, dict_logical_disk_insert_cb, NULL);

    physicalDisks = dictionary_create_advanced(DICT_OPTION_DONT_OVERWRITE_VALUE |
                                                  DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct physical_disk));

    dictionary_register_insert_callback(physicalDisks, dict_physical_disk_insert_cb, NULL);
}

static STRING *getFileSystemType(const char* diskName) {
    if (!diskName || !*diskName) return NULL;

    char fileSystemNameBuffer[128] = {0};  // Buffer for file system name
    char pathBuffer[256] = {0};            // Path buffer to accommodate different formats
    DWORD serialNumber = 0;
    DWORD maxComponentLength = 0;
    DWORD fileSystemFlags = 0;
    BOOL success;

    // Check if the input is likely a drive letter (e.g., "C:")
    if (isalpha((uint8_t)diskName[0]) && diskName[1] == ':' && diskName[2] == '\0')
        snprintf(pathBuffer, sizeof(pathBuffer), "%s\\", diskName);  // Format as "C:\\"
    else
        // Assume it's a Volume GUID path or a device path
        snprintf(pathBuffer, sizeof(pathBuffer), "\\\\.\\%s", diskName);  // Format as "\\.\HarddiskVolume1"

    // Attempt to get the volume information
    success = GetVolumeInformation(
        pathBuffer,                // Path to the disk
        NULL,                      // We don't need the volume name
        0,                         // Size of volume name buffer is 0
        &serialNumber,             // Volume serial number
        &maxComponentLength,       // Maximum component length
        &fileSystemFlags,          // File system flags
        fileSystemNameBuffer,      // File system name buffer
        sizeof(fileSystemNameBuffer) // Size of file system name buffer
    );

    if (success && fileSystemNameBuffer[0]) {
        char *s = fileSystemNameBuffer;
        while(*s) { *s = tolower((uint8_t)*s); s++; }
        return string_strdupz(fileSystemNameBuffer);  // Duplicate the file system name
    }
    else
        return NULL;
}

static bool do_logical_disk(PERF_DATA_BLOCK *pDataBlock, int update_every, usec_t now_ut) {
    DICTIONARY *dict = logicalDisks;

    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "LogicalDisk");
    if(!pObjectType) return false;

    PERF_INSTANCE_DEFINITION *pi = NULL;
    for(LONG i = 0; i < pObjectType->NumInstances ; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if(!pi) break;

        if(!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        if(strcasecmp(windows_shared_buffer, "_Total") == 0)
            continue;

        struct logical_disk *d = dictionary_set(dict, windows_shared_buffer, NULL, sizeof(*d));
        d->last_collected = now_ut;

        if(!d->collected_metadata) {
            d->filesystem = getFileSystemType(windows_shared_buffer);
            d->collected_metadata = true;
        }

        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->percentDiskFree);
        // perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->freeMegabytes);

        if(!d->st_disk_space) {
            d->st_disk_space = rrdset_create_localhost(
                "disk_space"
                , windows_shared_buffer
                , NULL
                , windows_shared_buffer
                , "disk.space"
                , "Disk Space Usage"
                , "GiB"
                , PLUGIN_WINDOWS_NAME
                , "PerflibStorage"
                , NETDATA_CHART_PRIO_DISKSPACE_SPACE
                , update_every
                , RRDSET_TYPE_STACKED
            );

            rrdlabels_add(d->st_disk_space->rrdlabels, "mount_point", windows_shared_buffer, RRDLABEL_SRC_AUTO);
            // rrdlabels_add(d->st->rrdlabels, "mount_root", name, RRDLABEL_SRC_AUTO);

            if(d->filesystem)
                rrdlabels_add(d->st_disk_space->rrdlabels, "filesystem", string2str(d->filesystem), RRDLABEL_SRC_AUTO);

            d->rd_disk_space_free = rrddim_add(d->st_disk_space, "avail", NULL, 1, 1024, RRD_ALGORITHM_ABSOLUTE);
            d->rd_disk_space_used = rrddim_add(d->st_disk_space, "used", NULL, 1, 1024, RRD_ALGORITHM_ABSOLUTE);
        }

        // percentDiskFree has the free space in Data and the size of the disk in Time, in MiB.
        rrddim_set_by_pointer(d->st_disk_space, d->rd_disk_space_free, (collected_number)d->percentDiskFree.current.Data);
        rrddim_set_by_pointer(d->st_disk_space, d->rd_disk_space_used, (collected_number)(d->percentDiskFree.current.Time - d->percentDiskFree.current.Data));
        rrdset_done(d->st_disk_space);
    }

    // cleanup
    {
        struct logical_disk *d;
        dfe_start_write(dict, d) {
            if(d->last_collected < now_ut) {
                logical_disk_cleanup(d);
                dictionary_del(dict, d_dfe.name);
            }
        }
        dfe_done(d);
        dictionary_garbage_collect(dict);
    }

    return true;
}

static void physical_disk_labels(RRDSET *st, void *data) {
    struct physical_disk *d = data;

    if(d->device)
        rrdlabels_add(st->rrdlabels, "device", string2str(d->device), RRDLABEL_SRC_AUTO);

    if (d->mount_point)
        rrdlabels_add(st->rrdlabels, "mount_point", string2str(d->mount_point), RRDLABEL_SRC_AUTO);
}

static bool str_is_numeric(const char *s) {
    while(*s) if(!isdigit((uint8_t)*s++)) return false;
    return true;
}

static bool do_physical_disk(PERF_DATA_BLOCK *pDataBlock, int update_every, usec_t now_ut) {
    DICTIONARY *dict = physicalDisks;

    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "PhysicalDisk");
    if(!pObjectType) return false;

    PERF_INSTANCE_DEFINITION *pi = NULL;
    for (LONG i = 0; i < pObjectType->NumInstances; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if (!pi)
            break;

        if (!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        char *device = windows_shared_buffer;
        char mount_point[128]; mount_point[0] = '\0';

        struct physical_disk *d;
        bool is_system;
        if (strcasecmp(windows_shared_buffer, "_Total") == 0) {
            d = &system_physical_total;
            is_system = true;
        }
        else {
            char *space;
            if((space = strchr(windows_shared_buffer, ' '))) {
                *space++ = '\0';
                strncpyz(mount_point, space, sizeof(mount_point) - 1);
            }

            if(str_is_numeric(windows_shared_buffer)) {
                uint64_t n = str2ull(device, NULL);
                snprintfz(windows_shared_buffer, sizeof(windows_shared_buffer), "Disk %" PRIu64, n);
                device = windows_shared_buffer;
            }

            d = dictionary_set(dict, device, NULL, sizeof(*d));
            is_system = false;
        }
        d->last_collected = now_ut;

        if (!d->collected_metadata) {
            // TODO collect metadata - device_type, serial, id
            d->device = string_strdupz(device);
            d->mount_point = string_strdupz(mount_point);
            d->collected_metadata = true;
        }

        if (perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->diskReadBytesPerSec) &&
            perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->diskWriteBytesPerSec)) {
            if(is_system)
                common_system_io(
                        d->diskReadBytesPerSec.current.Data,
                        d->diskWriteBytesPerSec.current.Data,
                        update_every);
            else
                common_disk_io(
                    &d->disk_io,
                    device,
                    NULL,
                    d->diskReadBytesPerSec.current.Data,
                    d->diskWriteBytesPerSec.current.Data,
                    update_every,
                    physical_disk_labels,
                    d);
        }

        if(is_system) continue;

        if (perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->diskReadsPerSec) &&
            perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->diskWritesPerSec)) {

            common_disk_ops(
                    &d->disk_ops,
                    device,
                    NULL,
                    d->diskReadBytesPerSec.current.Data,
                    d->diskWriteBytesPerSec.current.Data,
                    update_every,
                    physical_disk_labels,
                    d);
        }

        if (perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->percentIdleTime)) {
            if (d->percentIdleTime.previous.Data && d->percentIdleTime.previous.Time &&
                    d->percentIdleTime.current.Time > d->percentIdleTime.previous.Time) {
                collected_number idle_percentage =
                        100 * (d->percentIdleTime.current.Data - d->percentIdleTime.previous.Data)
                        / (d->percentIdleTime.current.Time - d->percentIdleTime.previous.Time);

                if (idle_percentage > 100)
                    idle_percentage = 100;

                common_disk_util(
                        &d->disk_util,
                        device,
                        NULL,
                        100 - idle_percentage,
                        update_every,
                        physical_disk_labels,
                        d);
            }
        }

        if (perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->percentDiskTime)) {
            common_disk_busy(
                    &d->disk_busy,
                    device,
                    NULL,
                    d->percentDiskTime.current.Data / NS100_PER_MS,
                    update_every,
                    physical_disk_labels,
                    d);
        }

        if (perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->percentDiskReadTime) &&
            perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->percentDiskWriteTime)) {

            common_disk_iotime(
                    &d->disk_iotime,
                    device,
                    NULL,
                    d->percentDiskReadTime.current.Data / NS100_PER_MS,
                    d->percentDiskWriteTime.current.Data / NS100_PER_MS,
                    update_every,
                    physical_disk_labels,
                    d);
        }

        if(perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->currentDiskQueueLength)) {
            common_disk_qops(
                    &d->disk_qops,
                    device,
                    NULL,
                    d->currentDiskQueueLength.current.Data,
                    update_every,
                    physical_disk_labels,
                    d);
        }

        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskQueueLength);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskReadQueueLength);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskWriteQueueLength);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskSecondsPerTransfer);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskSecondsPerRead);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskSecondsPerWrite);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->diskTransfersPerSec);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->diskBytesPerSec);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskBytesPerTransfer);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskBytesPerRead);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->averageDiskBytesPerWrite);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &d->splitIoPerSec);
    }

    // cleanup
    {
        struct physical_disk *d;
        dfe_start_write(dict, d) {
            if(d->last_collected < now_ut) {
                physical_disk_cleanup(d);
                dictionary_del(dict, d_dfe.name);
            }
        }
        dfe_done(d);
        dictionary_garbage_collect(dict);
    }

    return true;
}

int do_PerflibStorage(int update_every, usec_t dt __maybe_unused) {
    static bool initialized = false;

    if(unlikely(!initialized)) {
        initialize();
        initialized = true;
    }

    DWORD id = RegistryFindIDByName("LogicalDisk");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    usec_t now_ut = now_monotonic_usec();
    do_logical_disk(pDataBlock, update_every, now_ut);
    do_physical_disk(pDataBlock, update_every, now_ut);

    return 0;
}
