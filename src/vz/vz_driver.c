/*
 * vz_driver.c: core driver functions for managing
 * Parallels Cloud Server hosts
 *
 * Copyright (C) 2014-2015 Red Hat, Inc.
 * Copyright (C) 2012 Parallels, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include <sys/types.h>
#include <sys/poll.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/statvfs.h>

#include "datatypes.h"
#include "virerror.h"
#include "viralloc.h"
#include "virlog.h"
#include "vircommand.h"
#include "configmake.h"
#include "virfile.h"
#include "virstoragefile.h"
#include "nodeinfo.h"
#include "virstring.h"
#include "cpu/cpu.h"
#include "virtypedparam.h"

#include "vz_driver.h"
#include "vz_utils.h"
#include "vz_sdk.h"

#define VIR_FROM_THIS VIR_FROM_PARALLELS

VIR_LOG_INIT("parallels.parallels_driver");

#define PRLCTL                      "prlctl"

static virClassPtr vzDriverClass;

static virMutex vz_driver_lock;
static vzDriverPtr vz_driver;
static vzConnPtr vz_conn_list;

static vzDriverPtr
vzDriverObjNew(void);

static int
vzCapsAddGuestDomain(virCapsPtr caps,
                     virDomainOSType ostype,
                     virArch arch,
                     const char * emulator,
                     virDomainVirtType virt_type)
{
    virCapsGuestPtr guest;

    if ((guest = virCapabilitiesAddGuest(caps, ostype, arch, emulator,
                                         NULL, 0, NULL)) == NULL)
        return -1;


    if (virCapabilitiesAddGuestDomain(guest, virt_type,
                                      NULL, NULL, 0, NULL) == NULL)
        return -1;

    return 0;
}

static virCapsPtr
vzBuildCapabilities(void)
{
    virCapsPtr caps = NULL;
    virCPUDefPtr cpu = NULL;
    virCPUDataPtr data = NULL;
    virNodeInfo nodeinfo;
    virDomainOSType ostypes[] = {
        VIR_DOMAIN_OSTYPE_HVM,
        VIR_DOMAIN_OSTYPE_EXE
    };
    virArch archs[] = { VIR_ARCH_I686, VIR_ARCH_X86_64 };
    const char *const emulators[] = { "vz", "parallels"};
    virDomainVirtType virt_types[] = {
        VIR_DOMAIN_VIRT_VZ,
        VIR_DOMAIN_VIRT_PARALLELS
    };
    size_t i, j, k;

    if ((caps = virCapabilitiesNew(virArchFromHost(),
                                   false, false)) == NULL)
        return NULL;

    if (nodeCapsInitNUMA(caps) < 0)
        goto error;

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            for (k = 0; k < 2; k++)
                if (vzCapsAddGuestDomain(caps, ostypes[i], archs[j],
                                         emulators[k], virt_types[k]) < 0)
                    goto error;

    if (nodeGetInfo(&nodeinfo))
        goto error;

    if (VIR_ALLOC(cpu) < 0)
        goto error;

    cpu->arch = caps->host.arch;
    cpu->type = VIR_CPU_TYPE_HOST;
    cpu->sockets = nodeinfo.sockets;
    cpu->cores = nodeinfo.cores;
    cpu->threads = nodeinfo.threads;

    caps->host.cpu = cpu;

    if (virCapabilitiesAddHostMigrateTransport(caps, "vzmigr") < 0)
        goto error;

    if (!(data = cpuNodeData(cpu->arch))
        || cpuDecode(cpu, data, NULL, 0, NULL) < 0) {
        goto cleanup;
    }

 cleanup:
    cpuDataFree(data);
    return caps;

 error:
    virObjectUnref(caps);
    goto cleanup;
}

static void vzDriverDispose(void * obj)
{
    vzDriverPtr driver = obj;

    if (driver->server) {
        prlsdkUnsubscribeFromPCSEvents(driver);
        prlsdkDisconnect(driver);
    }

    virObjectUnref(driver->domains);
    virObjectUnref(driver->caps);
    virObjectUnref(driver->xmlopt);
    virObjectEventStateFree(driver->domainEventState);
    virSysinfoDefFree(driver->hostsysinfo);
}

static int vzDriverOnceInit(void)
{
    if (!(vzDriverClass = virClassNew(virClassForObjectLockable(),
                                      "vzDriver",
                                      sizeof(vzDriver),
                                      vzDriverDispose)))
        return -1;

    return 0;
}
VIR_ONCE_GLOBAL_INIT(vzDriver)

vzDriverPtr
vzGetDriverConnection(void)
{
    virMutexLock(&vz_driver_lock);
    if (!vz_driver)
        vz_driver = vzDriverObjNew();
    virObjectRef(vz_driver);
    virMutexUnlock(&vz_driver_lock);

    return vz_driver;
}

void
vzDestroyDriverConnection(void)
{

    vzDriverPtr driver;
    vzConnPtr privconn_list;

    virMutexLock(&vz_driver_lock);
    driver = vz_driver;
    vz_driver = NULL;

    privconn_list = vz_conn_list;
    vz_conn_list = NULL;

    virMutexUnlock(&vz_driver_lock);

    while (privconn_list) {
        vzConnPtr privconn = privconn_list;
        privconn_list = privconn->next;
        virConnectCloseCallbackDataCall(privconn->closeCallback,
                                        VIR_CONNECT_CLOSE_REASON_EOF);
    }
    virObjectUnref(driver);
}

static char *
vzConnectGetCapabilities(virConnectPtr conn)
{
    vzConnPtr privconn = conn->privateData;
    char *xml;

    xml = virCapabilitiesFormatXML(privconn->driver->caps);
    return xml;
}
static int
vzDomainDefAddDefaultInputDevices(virDomainDefPtr def)
{
    if (def->ngraphics == 0)
        return 0;

    int bus = IS_CT(def) ? VIR_DOMAIN_INPUT_BUS_PARALLELS :
                           VIR_DOMAIN_INPUT_BUS_PS2;

    if (virDomainDefMaybeAddInput(def,
                                  VIR_DOMAIN_INPUT_TYPE_MOUSE,
                                  bus) < 0)
        return -1;

    if (virDomainDefMaybeAddInput(def,
                                  VIR_DOMAIN_INPUT_TYPE_KBD,
                                  bus) < 0)
        return -1;

    return 0;
}

static int
vzDomainDefPostParse(virDomainDefPtr def,
                     virCapsPtr caps ATTRIBUTE_UNUSED,
                     unsigned int parseFlags ATTRIBUTE_UNUSED,
                     void *opaque)
{
    if (vzDomainDefAddDefaultInputDevices(def) < 0)
        return -1;

    if (vzCheckUnsupportedDisks(def, opaque) < 0)
        return -1;

    if (vzCheckUnsupportedControllers(def, opaque) < 0)
        return -1;

    return 0;
}

static int
vzDomainDeviceDefPostParse(virDomainDeviceDefPtr dev,
                           const virDomainDef *def,
                           virCapsPtr caps ATTRIBUTE_UNUSED,
                           unsigned int parseFlags ATTRIBUTE_UNUSED,
                           void *opaque ATTRIBUTE_UNUSED)
{
    int ret = -1;

    if (dev->type == VIR_DOMAIN_DEVICE_NET &&
        (dev->data.net->type == VIR_DOMAIN_NET_TYPE_NETWORK ||
         dev->data.net->type == VIR_DOMAIN_NET_TYPE_BRIDGE) &&
        !dev->data.net->model &&
        def->os.type == VIR_DOMAIN_OSTYPE_HVM &&
        VIR_STRDUP(dev->data.net->model, "e1000") < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    return ret;
}


static virDomainDefParserConfig vzDomainDefParserConfig = {
    .macPrefix = {0x42, 0x1C, 0x00},
    .devicesPostParseCallback = vzDomainDeviceDefPostParse,
    .domainPostParseCallback = vzDomainDefPostParse,
};

static vzDriverPtr
vzDriverObjNew(void)
{
    vzDriverPtr driver;

    if (vzDriverInitialize() < 0)
        return NULL;

    if (!(driver = virObjectLockableNew(vzDriverClass)))
        return NULL;

    vzDomainDefParserConfig.priv = &driver->vzCaps;

    if (!(driver->caps = vzBuildCapabilities()) ||
        !(driver->xmlopt = virDomainXMLOptionNew(&vzDomainDefParserConfig,
                                                   NULL, NULL)) ||
        !(driver->domains = virDomainObjListNew()) ||
        !(driver->domainEventState = virObjectEventStateNew()) ||
        (vzInitVersion(driver) < 0) ||
        (prlsdkConnect(driver) < 0) ||
        (prlsdkSubscribeToPCSEvents(driver) < 0)
        ) {
        virObjectUnref(driver);
        return NULL;
    }

    driver->hostsysinfo = virSysinfoRead();
    ignore_value(prlsdkLoadDomains(driver));
    return driver;
}

static virDrvOpenStatus
vzConnectOpen(virConnectPtr conn,
              virConnectAuthPtr auth ATTRIBUTE_UNUSED,
              virConfPtr conf ATTRIBUTE_UNUSED,
              unsigned int flags)
{
    vzDriverPtr driver = NULL;
    vzConnPtr privconn = NULL;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (!conn->uri)
        return VIR_DRV_OPEN_DECLINED;

    if (!conn->uri->scheme)
        return VIR_DRV_OPEN_DECLINED;

    if (STRNEQ(conn->uri->scheme, "vz") &&
        STRNEQ(conn->uri->scheme, "parallels"))
        return VIR_DRV_OPEN_DECLINED;

    if (STREQ(conn->uri->scheme, "vz") && STRNEQ(conn->driver->name, "vz"))
        return VIR_DRV_OPEN_DECLINED;

    if (STREQ(conn->uri->scheme, "parallels") && STRNEQ(conn->driver->name, "Parallels"))
        return VIR_DRV_OPEN_DECLINED;

    /* Remote driver should handle these. */
    if (conn->uri->server)
        return VIR_DRV_OPEN_DECLINED;

    /* From this point on, the connection is for us. */
    if (STRNEQ_NULLABLE(conn->uri->path, "/system")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected Virtuozzo URI path '%s', try vz:///system"),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }

    if (!(driver = vzGetDriverConnection()))
        return VIR_DRV_OPEN_ERROR;

    if (VIR_ALLOC(privconn) < 0)
        goto error;

    conn->privateData = privconn;
    privconn->driver = driver;

    if (!(privconn->closeCallback = virNewConnectCloseCallbackData()))
        goto error;

    virMutexLock(&vz_driver_lock);
    privconn->next = vz_conn_list;
    vz_conn_list = privconn;
    virMutexUnlock(&vz_driver_lock);

    return VIR_DRV_OPEN_SUCCESS;

 error:

    conn->privateData = NULL;
    virObjectUnref(driver);
    VIR_FREE(privconn);
    return VIR_DRV_OPEN_ERROR;
}

static int
vzConnectClose(virConnectPtr conn)
{
    vzConnPtr curr, *prev = &vz_conn_list;
    vzConnPtr privconn = conn->privateData;

    if (!privconn)
        return 0;

    virMutexLock(&vz_driver_lock);
    for (curr = vz_conn_list; curr; prev = &curr->next, curr = curr->next) {
        if (curr == privconn) {
            *prev = curr->next;
            break;
        }
    }

    virMutexUnlock(&vz_driver_lock);

    virObjectUnref(privconn->closeCallback);
    virObjectUnref(privconn->driver);
    VIR_FREE(privconn);
    conn->privateData = NULL;
    return 0;
}

static int
vzConnectGetVersion(virConnectPtr conn, unsigned long *hvVer)
{
    vzConnPtr privconn = conn->privateData;
    *hvVer = privconn->driver->vzVersion;
    return 0;
}


static char *vzConnectGetHostname(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return virGetHostname();
}

static char *
vzConnectGetSysinfo(virConnectPtr conn, unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    vzDriverPtr driver = privconn->driver;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, NULL);

    if (!driver->hostsysinfo) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Host SMBIOS information is not available"));
        return NULL;
    }

    if (virSysinfoFormat(&buf, driver->hostsysinfo) < 0)
        return NULL;
    if (virBufferCheckError(&buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}

static int
vzConnectListDomains(virConnectPtr conn, int *ids, int maxids)
{
    vzConnPtr privconn = conn->privateData;
    int n;

    n = virDomainObjListGetActiveIDs(privconn->driver->domains, ids, maxids,
                                     NULL, NULL);

    return n;
}

static int
vzConnectNumOfDomains(virConnectPtr conn)
{
    vzConnPtr privconn = conn->privateData;
    int count;

    count = virDomainObjListNumOfDomains(privconn->driver->domains, true,
                                         NULL, NULL);

    return count;
}

static int
vzConnectListDefinedDomains(virConnectPtr conn, char **const names, int maxnames)
{
    vzConnPtr privconn = conn->privateData;
    int n;

    memset(names, 0, sizeof(*names) * maxnames);
    n = virDomainObjListGetInactiveNames(privconn->driver->domains, names,
                                         maxnames, NULL, NULL);

    return n;
}

static int
vzConnectNumOfDefinedDomains(virConnectPtr conn)
{
    vzConnPtr privconn = conn->privateData;
    int count;

    count = virDomainObjListNumOfDomains(privconn->driver->domains, false,
                                         NULL, NULL);
    return count;
}

static int
vzConnectListAllDomains(virConnectPtr conn,
                        virDomainPtr **domains,
                        unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);
    ret = virDomainObjListExport(privconn->driver->domains, conn, domains,
                                 NULL, flags);

    return ret;
}

static virDomainPtr
vzDomainLookupByID(virConnectPtr conn, int id)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr ret = NULL;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByID(privconn->driver->domains, id);

    if (dom == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    ret = virGetDomain(conn, dom->def->name, dom->def->uuid);
    if (ret)
        ret->id = dom->def->id;

 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return ret;
}

static virDomainPtr
vzDomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr ret = NULL;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByUUID(privconn->driver->domains, uuid);

    if (dom == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    ret = virGetDomain(conn, dom->def->name, dom->def->uuid);
    if (ret)
        ret->id = dom->def->id;

 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return ret;
}

static virDomainPtr
vzDomainLookupByName(virConnectPtr conn, const char *name)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr ret = NULL;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByName(privconn->driver->domains, name);

    if (dom == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), name);
        goto cleanup;
    }

    ret = virGetDomain(conn, dom->def->name, dom->def->uuid);
    if (ret)
        ret->id = dom->def->id;

 cleanup:
    virDomainObjEndAPI(&dom);
    return ret;
}

static int
vzDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    virDomainObjPtr privdom;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomainRef(domain)))
        goto cleanup;

    info->state = virDomainObjGetState(privdom, NULL);
    info->memory = privdom->def->mem.cur_balloon;
    info->maxMem = virDomainDefGetMemoryActual(privdom->def);
    info->nrVirtCpu = virDomainDefGetVcpus(privdom->def);
    info->cpuTime = 0;

    if (virDomainObjIsActive(privdom)) {
        unsigned long long vtime;
        size_t i;

        for (i = 0; i < virDomainDefGetVcpus(privdom->def); ++i) {
            if (prlsdkGetVcpuStats(privdom, i, &vtime) < 0) {
                virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                               _("cannot read cputime for domain"));
                goto cleanup;
            }
            info->cpuTime += vtime;
        }
    }
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&privdom);
    return ret;
}

static char *
vzDomainGetOSType(virDomainPtr domain)
{
    virDomainObjPtr privdom;

    char *ret = NULL;

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    ignore_value(VIR_STRDUP(ret, virDomainOSTypeToString(privdom->def->os.type)));

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int
vzDomainIsPersistent(virDomainPtr domain)
{
    virDomainObjPtr privdom;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    ret = 1;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int
vzDomainGetState(virDomainPtr domain,
                 int *state, int *reason, unsigned int flags)
{
    virDomainObjPtr privdom;
    int ret = -1;
    virCheckFlags(0, -1);

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    *state = virDomainObjGetState(privdom, reason);
    ret = 0;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static char *
vzDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    vzConnPtr privconn = domain->conn->privateData;
    virDomainDefPtr def;
    virDomainObjPtr privdom;
    char *ret = NULL;

    /* Flags checked by virDomainDefFormat */

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    def = (flags & VIR_DOMAIN_XML_INACTIVE) &&
        privdom->newDef ? privdom->newDef : privdom->def;

    ret = virDomainDefFormat(def, privconn->driver->caps, flags);

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int
vzDomainGetAutostart(virDomainPtr domain, int *autostart)
{
    virDomainObjPtr privdom;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    *autostart = privdom->autostart;
    ret = 0;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static virDomainPtr
vzDomainDefineXMLFlags(virConnectPtr conn, const char *xml, unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr retdom = NULL;
    virDomainDefPtr def;
    virDomainObjPtr olddom = NULL;
    virDomainObjPtr newdom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;
    vzDriverPtr driver = privconn->driver;

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_DEFINE_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    virObjectLock(driver);
    if ((def = virDomainDefParseString(xml, driver->caps, driver->xmlopt,
                                       parse_flags)) == NULL)
        goto cleanup;

    olddom = virDomainObjListFindByUUID(driver->domains, def->uuid);
    if (olddom == NULL) {
        virResetLastError();
        newdom = vzNewDomain(driver, def->name, def->uuid);
        if (!newdom)
            goto cleanup;
        if (def->os.type == VIR_DOMAIN_OSTYPE_HVM) {
            if (prlsdkCreateVm(driver, def))
                goto cleanup;
        } else if (def->os.type == VIR_DOMAIN_OSTYPE_EXE) {
            if (prlsdkCreateCt(driver, def))
                goto cleanup;
        } else {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("Unsupported OS type: %s"),
                           virDomainOSTypeToString(def->os.type));
            goto cleanup;
        }

        if (prlsdkLoadDomain(driver, newdom))
            goto cleanup;
    } else {
        int state, reason;

        state = virDomainObjGetState(olddom, &reason);

        if (state == VIR_DOMAIN_SHUTOFF &&
            reason == VIR_DOMAIN_SHUTOFF_SAVED) {

            /* PCS doesn't store domain config in managed save state file.
             * It's forbidden to change config for VMs in this state.
             * It's possible to change config for containers, but after
             * restoring domain will have that new config, not a config,
             * which domain had at the moment of virDomainManagedSave.
             *
             * So forbid this operation, if config is changed. If it's
             * not changed - just do nothing. */

            if (!virDomainDefCheckABIStability(olddom->def, def)) {
                virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                               _("Can't change domain configuration "
                                 "in managed save state"));
                goto cleanup;
            }
        } else {
            if (prlsdkApplyConfig(driver, olddom, def))
                goto cleanup;

            if (prlsdkUpdateDomain(driver, olddom))
                goto cleanup;
        }
    }

    retdom = virGetDomain(conn, def->name, def->uuid);
    if (retdom)
        retdom->id = def->id;

 cleanup:
    if (olddom)
        virObjectUnlock(olddom);
    if (newdom) {
        if (!retdom)
             virDomainObjListRemove(driver->domains, newdom);
        else
             virObjectUnlock(newdom);
    }
    virDomainDefFree(def);
    virObjectUnlock(driver);
    return retdom;
}

static virDomainPtr
vzDomainDefineXML(virConnectPtr conn, const char *xml)
{
    return vzDomainDefineXMLFlags(conn, xml, 0);
}


static int
vzNodeGetInfo(virConnectPtr conn ATTRIBUTE_UNUSED,
              virNodeInfoPtr nodeinfo)
{
    return nodeGetInfo(nodeinfo);
}

static int vzConnectIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Encryption is not relevant / applicable to way we talk to PCS */
    return 0;
}

static int vzConnectIsSecure(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* We run CLI tools directly so this is secure */
    return 1;
}

static int vzConnectIsAlive(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 1;
}


static char *
vzConnectBaselineCPU(virConnectPtr conn ATTRIBUTE_UNUSED,
                     const char **xmlCPUs,
                     unsigned int ncpus,
                     unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES, NULL);

    return cpuBaselineXML(xmlCPUs, ncpus, NULL, 0, flags);
}


static int
vzDomainGetVcpus(virDomainPtr domain,
                 virVcpuInfoPtr info,
                 int maxinfo,
                 unsigned char *cpumaps,
                 int maplen)
{
    virDomainObjPtr privdom = NULL;
    size_t i;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomainRef(domain)))
        goto cleanup;

    if (!virDomainObjIsActive(privdom)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s",
                       _("cannot list vcpu pinning for an inactive domain"));
        goto cleanup;
    }

    if (maxinfo >= 1) {
        if (info != NULL) {
            memset(info, 0, sizeof(*info) * maxinfo);
            for (i = 0; i < maxinfo; i++) {
                info[i].number = i;
                info[i].state = VIR_VCPU_RUNNING;
                if (prlsdkGetVcpuStats(privdom, i, &info[i].cpuTime) < 0)
                    goto cleanup;
            }
        }
        if (cpumaps != NULL) {
            memset(cpumaps, 0, maplen * maxinfo);
            for (i = 0; i < maxinfo; i++)
                virBitmapToDataBuf(privdom->def->cpumask,
                                   VIR_GET_CPUMAP(cpumaps, maplen, i),
                                   maplen);
        }
    }
    ret = maxinfo;

 cleanup:
    if (privdom)
        virDomainObjEndAPI(&privdom);
    return ret;
}


static int
vzNodeGetCPUMap(virConnectPtr conn ATTRIBUTE_UNUSED,
                unsigned char **cpumap,
                unsigned int *online,
                unsigned int flags)
{
    return nodeGetCPUMap(cpumap, online, flags);
}

static int
vzConnectDomainEventRegisterAny(virConnectPtr conn,
                                virDomainPtr domain,
                                int eventID,
                                virConnectDomainEventGenericCallback callback,
                                void *opaque,
                                virFreeCallback freecb)
{
    int ret = -1;
    vzConnPtr privconn = conn->privateData;
    if (virDomainEventStateRegisterID(conn,
                                      privconn->driver->domainEventState,
                                      domain, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;
    return ret;
}

static int
vzConnectDomainEventDeregisterAny(virConnectPtr conn,
                                  int callbackID)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;

    if (virObjectEventStateDeregisterID(conn,
                                        privconn->driver->domainEventState,
                                        callbackID) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    return ret;
}

static int vzDomainSuspend(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkPause);
}

static int vzDomainResume(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkResume);
}

static int vzDomainCreate(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkStart);
}

static int vzDomainDestroy(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkKill);
}

static int vzDomainShutdown(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkStop);
}

static int vzDomainReboot(virDomainPtr domain,
                          unsigned int flags)
{
    virCheckFlags(0, -1);
    return prlsdkDomainChangeState(domain, prlsdkRestart);
}

static int vzDomainIsActive(virDomainPtr domain)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    ret = virDomainObjIsActive(dom);
    virObjectUnlock(dom);

    return ret;
}

static int
vzDomainCreateWithFlags(virDomainPtr domain, unsigned int flags)
{
    /* we don't support any create flags */
    virCheckFlags(0, -1);

    return vzDomainCreate(domain);
}

static int
vzDomainUndefineFlags(virDomainPtr domain,
                      unsigned int flags)
{
    vzConnPtr privconn = domain->conn->privateData;
    virDomainObjPtr dom = NULL;
    int ret;

    virCheckFlags(VIR_DOMAIN_UNDEFINE_MANAGED_SAVE |
                  VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    ret = prlsdkUnregisterDomain(privconn->driver, dom, flags);
    if (ret)
        virObjectUnlock(dom);

    return ret;
}

static int
vzDomainUndefine(virDomainPtr domain)
{
    return vzDomainUndefineFlags(domain, 0);
}

static int
vzDomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom = NULL;
    int state, reason;
    int ret = 0;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    state = virDomainObjGetState(dom, &reason);
    if (state == VIR_DOMAIN_SHUTOFF && reason == VIR_DOMAIN_SHUTOFF_SAVED)
        ret = 1;
    virObjectUnlock(dom);

    return ret;
}

static int
vzDomainManagedSave(virDomainPtr domain, unsigned int flags)
{
    vzConnPtr privconn = domain->conn->privateData;
    virDomainObjPtr dom = NULL;
    int state, reason;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_SAVE_RUNNING |
                  VIR_DOMAIN_SAVE_PAUSED, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    state = virDomainObjGetState(dom, &reason);

    if (state == VIR_DOMAIN_RUNNING && (flags & VIR_DOMAIN_SAVE_PAUSED)) {
        ret = prlsdkDomainChangeStateLocked(privconn->driver, dom, prlsdkPause);
        if (ret)
            goto cleanup;
    }

    ret = prlsdkDomainChangeStateLocked(privconn->driver, dom, prlsdkSuspend);

 cleanup:
    virObjectUnlock(dom);
    return ret;
}

static int
vzDomainManagedSaveRemove(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom = NULL;
    int state, reason;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    state = virDomainObjGetState(dom, &reason);

    if (!(state == VIR_DOMAIN_SHUTOFF && reason == VIR_DOMAIN_SHUTOFF_SAVED))
        goto cleanup;

    ret = prlsdkDomainManagedSaveRemove(dom);

 cleanup:
    virObjectUnlock(dom);
    return ret;
}

static int vzCheckConfigUpdateFlags(virDomainObjPtr dom, unsigned int *flags)
{
    if (virDomainObjUpdateModificationImpact(dom, flags) < 0)
        return -1;

    if (!(*flags & VIR_DOMAIN_AFFECT_CONFIG)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain config update needs VIR_DOMAIN_AFFECT_CONFIG "
                         "flag to be set"));
        return -1;
    }

    if (virDomainObjIsActive(dom) && !(*flags & VIR_DOMAIN_AFFECT_LIVE)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Updates on a running domain need "
                         "VIR_DOMAIN_AFFECT_LIVE flag"));
        return -1;
    }

    return 0;
}

static int vzDomainAttachDeviceFlags(virDomainPtr dom, const char *xml,
                                     unsigned int flags)
{
    int ret = -1;
    vzConnPtr privconn = dom->conn->privateData;
    virDomainDeviceDefPtr dev = NULL;
    virDomainObjPtr privdom = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(privdom = vzDomObjFromDomain(dom)))
        return -1;

    if (vzCheckConfigUpdateFlags(privdom, &flags) < 0)
        goto cleanup;

    dev = virDomainDeviceDefParse(xml, privdom->def, privconn->driver->caps,
                                  privconn->driver->xmlopt, VIR_DOMAIN_XML_INACTIVE);
    if (dev == NULL)
        goto cleanup;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        ret = prlsdkAttachVolume(privconn->driver, privdom, dev->data.disk);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("disk attach failed"));
            goto cleanup;
        }
        break;
    case VIR_DOMAIN_DEVICE_NET:
        ret = prlsdkAttachNet(privconn->driver, privdom, dev->data.net);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("network attach failed"));
            goto cleanup;
        }
        break;
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("device type '%s' cannot be attached"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    ret = 0;
 cleanup:
    virObjectUnlock(privdom);
    return ret;
}

static int vzDomainAttachDevice(virDomainPtr dom, const char *xml)
{
    return vzDomainAttachDeviceFlags(dom, xml,
                                     VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_AFFECT_LIVE);
}

static int vzDomainDetachDeviceFlags(virDomainPtr dom, const char *xml,
                                     unsigned int flags)
{
    int ret = -1;
    vzConnPtr privconn = dom->conn->privateData;
    virDomainDeviceDefPtr dev = NULL;
    virDomainObjPtr privdom = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    privdom = vzDomObjFromDomain(dom);
    if (privdom == NULL)
        return -1;

    if (vzCheckConfigUpdateFlags(privdom, &flags) < 0)
        goto cleanup;

    dev = virDomainDeviceDefParse(xml, privdom->def, privconn->driver->caps,
                                  privconn->driver->xmlopt,
                                  VIR_DOMAIN_XML_INACTIVE |
                                  VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE);
    if (dev == NULL)
        goto cleanup;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        ret = prlsdkDetachVolume(privdom, dev->data.disk);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("disk detach failed"));
            goto cleanup;
        }
        break;
    case VIR_DOMAIN_DEVICE_NET:
        ret = prlsdkDetachNet(privconn->driver, privdom, dev->data.net);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("network detach failed"));
            goto cleanup;
        }
        break;
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("device type '%s' cannot be detached"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    ret = 0;
 cleanup:
    virObjectUnlock(privdom);

    return ret;
}

static int vzDomainDetachDevice(virDomainPtr dom, const char *xml)
{
    return vzDomainDetachDeviceFlags(dom, xml,
                                     VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_AFFECT_LIVE);
}

static unsigned long long
vzDomainGetMaxMemory(virDomainPtr domain)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    ret = virDomainDefGetMemoryActual(dom->def);
    virObjectUnlock(dom);
    return ret;
}

static int
vzDomainBlockStats(virDomainPtr domain, const char *path,
                   virDomainBlockStatsPtr stats)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;
    size_t i;
    int idx;

    if (!(dom = vzDomObjFromDomainRef(domain)))
        return -1;

    if (*path) {
        if ((idx = virDomainDiskIndexByName(dom->def, path, false)) < 0) {
            virReportError(VIR_ERR_INVALID_ARG, _("invalid path: %s"), path);
            goto cleanup;
        }
        if (prlsdkGetBlockStats(dom, dom->def->disks[idx], stats) < 0)
            goto cleanup;
    } else {
        virDomainBlockStatsStruct s;

#define PARALLELS_ZERO_STATS(VAR, TYPE, NAME)      \
        stats->VAR = 0;

        PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_ZERO_STATS)

#undef PARALLELS_ZERO_STATS

        for (i = 0; i < dom->def->ndisks; i++) {
            if (prlsdkGetBlockStats(dom, dom->def->disks[i], &s) < 0)
                goto cleanup;

#define PARALLELS_SUM_STATS(VAR, TYPE, NAME)        \
    if (s.VAR != -1)                                \
        stats->VAR += s.VAR;

        PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_SUM_STATS)

#undef PARALLELS_SUM_STATS
        }
    }
    stats->errs = -1;
    ret = 0;

 cleanup:
    if (dom)
        virDomainObjEndAPI(&dom);

    return ret;
}

static int
vzDomainBlockStatsFlags(virDomainPtr domain,
                        const char *path,
                        virTypedParameterPtr params,
                        int *nparams,
                        unsigned int flags)
{
    virDomainBlockStatsStruct stats;
    int ret = -1;
    size_t i;

    virCheckFlags(VIR_TYPED_PARAM_STRING_OKAY, -1);
    /* We don't return strings, and thus trivially support this flag.  */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (vzDomainBlockStats(domain, path, &stats) < 0)
        goto cleanup;

    if (*nparams == 0) {
#define PARALLELS_COUNT_STATS(VAR, TYPE, NAME)       \
        if ((stats.VAR) != -1)                       \
            ++*nparams;

        PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_COUNT_STATS)

#undef PARALLELS_COUNT_STATS
        ret = 0;
        goto cleanup;
    }

    i = 0;
#define PARALLELS_BLOCK_STATS_ASSIGN_PARAM(VAR, TYPE, NAME)                    \
    if (i < *nparams && (stats.VAR) != -1) {                                   \
        if (virTypedParameterAssign(params + i, TYPE,                          \
                                    VIR_TYPED_PARAM_LLONG, (stats.VAR)) < 0)   \
            goto cleanup;                                                      \
        i++;                                                                   \
    }

    PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_BLOCK_STATS_ASSIGN_PARAM)

#undef PARALLELS_BLOCK_STATS_ASSIGN_PARAM

    *nparams = i;
    ret = 0;

 cleanup:
    return ret;
}

static int
vzDomainInterfaceStats(virDomainPtr domain,
                         const char *path,
                         virDomainInterfaceStatsPtr stats)
{
    virDomainObjPtr dom = NULL;
    int ret;

    if (!(dom = vzDomObjFromDomainRef(domain)))
        return -1;

    ret = prlsdkGetNetStats(dom, path, stats);
    virDomainObjEndAPI(&dom);

    return ret;
}

static int
vzDomainMemoryStats(virDomainPtr domain,
                    virDomainMemoryStatPtr stats,
                    unsigned int nr_stats,
                    unsigned int flags)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    virCheckFlags(0, -1);
    if (!(dom = vzDomObjFromDomainRef(domain)))
        return -1;

    ret = prlsdkGetMemoryStats(dom, stats, nr_stats);
    virDomainObjEndAPI(&dom);

    return ret;
}

static int
vzDomainGetVcpusFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virDomainObjPtr privdom = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);

    if (!(privdom = vzDomObjFromDomain(dom)))
        goto cleanup;

    if (flags & VIR_DOMAIN_VCPU_MAXIMUM)
        ret = virDomainDefGetVcpusMax(privdom->def);
    else
        ret = virDomainDefGetVcpus(privdom->def);

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);

    return ret;
}

static int vzDomainGetMaxVcpus(virDomainPtr dom)
{
    return vzDomainGetVcpusFlags(dom, (VIR_DOMAIN_AFFECT_LIVE |
                                       VIR_DOMAIN_VCPU_MAXIMUM));
}

static int vzDomainIsUpdated(virDomainPtr dom)
{
    virDomainObjPtr privdom;
    int ret = -1;

    /* As far as VZ domains are always updated (e.g. current==persistent),
     * we just check for domain existence */
    if (!(privdom = vzDomObjFromDomain(dom)))
        goto cleanup;

    ret = 0;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int vzConnectGetMaxVcpus(virConnectPtr conn ATTRIBUTE_UNUSED,
                                const char *type)
{
    /* As far as we have no limitation for containers
     * we report maximum */
    if (type == NULL || STRCASEEQ(type, "vz") || STRCASEEQ(type, "parallels"))
        return 1028;

    virReportError(VIR_ERR_INVALID_ARG,
                   _("unknown type '%s'"), type);
    return -1;
}

static int
vzNodeGetCPUStats(virConnectPtr conn ATTRIBUTE_UNUSED,
                  int cpuNum,
                  virNodeCPUStatsPtr params,
                  int *nparams,
                  unsigned int flags)
{
    return nodeGetCPUStats(cpuNum, params, nparams, flags);
}

static int
vzNodeGetMemoryStats(virConnectPtr conn ATTRIBUTE_UNUSED,
                     int cellNum,
                     virNodeMemoryStatsPtr params,
                     int *nparams,
                     unsigned int flags)
{
    return nodeGetMemoryStats(cellNum, params, nparams, flags);
}

static int
vzNodeGetCellsFreeMemory(virConnectPtr conn ATTRIBUTE_UNUSED,
                         unsigned long long *freeMems,
                         int startCell,
                         int maxCells)
{
    return nodeGetCellsFreeMemory(freeMems, startCell, maxCells);
}

static unsigned long long
vzNodeGetFreeMemory(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    unsigned long long freeMem;
    if (nodeGetMemory(NULL, &freeMem) < 0)
        return 0;
    return freeMem;
}

static int
vzConnectRegisterCloseCallback(virConnectPtr conn,
                               virConnectCloseFunc cb,
                               void *opaque,
                               virFreeCallback freecb)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;

    if (virConnectCloseCallbackDataGetCallback(privconn->closeCallback) != NULL) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("A close callback is already registered"));
        goto cleanup;
    }

    virConnectCloseCallbackDataRegister(privconn->closeCallback, conn, cb,
                                        opaque, freecb);
    ret = 0;

 cleanup:

    return ret;
}

static int
vzConnectUnregisterCloseCallback(virConnectPtr conn, virConnectCloseFunc cb)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;


    if (virConnectCloseCallbackDataGetCallback(privconn->closeCallback) != cb) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("A different callback was requested"));
        goto cleanup;
    }

    virConnectCloseCallbackDataUnregister(privconn->closeCallback, cb);
    ret = 0;

 cleanup:

    return ret;
}

static int vzDomainSetMemoryFlagsImpl(virDomainPtr domain, unsigned long memory,
                                      unsigned int flags, bool useflags)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    if (useflags && vzCheckConfigUpdateFlags(dom, &flags) < 0)
        goto cleanup;

    ret = prlsdkSetMemsize(dom, memory >> 10);

 cleanup:

    virObjectUnlock(dom);
    return ret;
}

static int vzDomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
                                  unsigned int flags)
{
    return vzDomainSetMemoryFlagsImpl(domain, memory, flags, true);
}

static int vzDomainSetMemory(virDomainPtr domain, unsigned long memory)
{
    return vzDomainSetMemoryFlagsImpl(domain, memory, 0, false);
}

static virDomainSnapshotObjPtr
vzSnapObjFromName(virDomainSnapshotObjListPtr snapshots, const char *name)
{
    virDomainSnapshotObjPtr snap = NULL;
    snap = virDomainSnapshotFindByName(snapshots, name);
    if (!snap)
        virReportError(VIR_ERR_NO_DOMAIN_SNAPSHOT,
                       _("no domain snapshot with matching name '%s'"), name);

    return snap;
}

static virDomainSnapshotObjPtr
vzSnapObjFromSnapshot(virDomainSnapshotObjListPtr snapshots,
                      virDomainSnapshotPtr snapshot)
{
    return vzSnapObjFromName(snapshots, snapshot->name);
}

static int
vzCurrentSnapshotIterator(void *payload,
                              const void *name ATTRIBUTE_UNUSED,
                              void *data)
{
    virDomainSnapshotObjPtr snapshot = payload;
    virDomainSnapshotObjPtr *current = data;

    if (snapshot->def->current)
        *current = snapshot;

    return 0;
}

static virDomainSnapshotObjPtr
vzFindCurrentSnapshot(virDomainSnapshotObjListPtr snapshots)
{
    virDomainSnapshotObjPtr current = NULL;

    virDomainSnapshotForEach(snapshots, vzCurrentSnapshotIterator, &current);
    return current;
}

static int
vzDomainSnapshotNum(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_ROOTS |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    n = virDomainSnapshotObjListNum(snapshots, NULL, flags);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return n;
}

static int
vzDomainSnapshotListNames(virDomainPtr domain,
                          char **names,
                          int nameslen,
                          unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_ROOTS |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    n = virDomainSnapshotObjListGetNames(snapshots, NULL, names, nameslen, flags);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return n;
}

static int
vzDomainListAllSnapshots(virDomainPtr domain,
                         virDomainSnapshotPtr **snaps,
                         unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_ROOTS |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    n = virDomainListSnapshots(snapshots, NULL, domain, snaps, flags);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return n;
}

static char *
vzDomainSnapshotGetXMLDesc(virDomainSnapshotPtr snapshot, unsigned int flags)
{
    virDomainObjPtr dom;
    char *xml = NULL;
    virDomainSnapshotObjPtr snap;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virDomainSnapshotObjListPtr snapshots = NULL;
    vzConnPtr privconn = snapshot->domain->conn->privateData;

    virCheckFlags(VIR_DOMAIN_XML_SECURE, NULL);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return NULL;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromSnapshot(snapshots, snapshot)))
        goto cleanup;

    virUUIDFormat(snapshot->domain->uuid, uuidstr);

    xml = virDomainSnapshotDefFormat(uuidstr, snap->def, privconn->driver->caps,
                                     virDomainDefFormatConvertXMLFlags(flags),
                                     0);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return xml;
}

static int
vzDomainSnapshotNumChildren(virDomainSnapshotPtr snapshot, unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjPtr snap;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromSnapshot(snapshots, snapshot)))
        goto cleanup;

    n = virDomainSnapshotObjListNum(snapshots, snap, flags);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return n;
}

static int
vzDomainSnapshotListChildrenNames(virDomainSnapshotPtr snapshot,
                                  char **names,
                                  int nameslen,
                                  unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjPtr snap;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromSnapshot(snapshots, snapshot)))
        goto cleanup;

    n = virDomainSnapshotObjListGetNames(snapshots, snap, names, nameslen, flags);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return n;
}

static int
vzDomainSnapshotListAllChildren(virDomainSnapshotPtr snapshot,
                                virDomainSnapshotPtr **snaps,
                                unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjPtr snap;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromSnapshot(snapshots, snapshot)))
        goto cleanup;

    n = virDomainListSnapshots(snapshots, snap, snapshot->domain, snaps, flags);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return n;
}

static virDomainSnapshotPtr
vzDomainSnapshotLookupByName(virDomainPtr domain,
                             const char *name,
                             unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjPtr snap;
    virDomainSnapshotPtr snapshot = NULL;
    virDomainSnapshotObjListPtr snapshots = NULL;

    virCheckFlags(0, NULL);

    if (!(dom = vzDomObjFromDomain(domain)))
        return NULL;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromName(snapshots, name)))
        goto cleanup;

    snapshot = virGetDomainSnapshot(domain, snap->def->name);

 cleanup:
    virObjectUnlock(dom);
    virDomainSnapshotObjListFree(snapshots);

    return snapshot;
}

static int
vzDomainHasCurrentSnapshot(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjListPtr snapshots = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    ret = vzFindCurrentSnapshot(snapshots) != NULL;

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return ret;
}

static virDomainSnapshotPtr
vzDomainSnapshotGetParent(virDomainSnapshotPtr snapshot, unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotObjPtr snap;
    virDomainSnapshotPtr parent = NULL;
    virDomainSnapshotObjListPtr snapshots = NULL;

    virCheckFlags(0, NULL);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return NULL;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromSnapshot(snapshots, snapshot)))
        goto cleanup;

    if (!snap->def->parent) {
        virReportError(VIR_ERR_NO_DOMAIN_SNAPSHOT,
                       _("snapshot '%s' does not have a parent"),
                       snap->def->name);
        goto cleanup;
    }

    parent = virGetDomainSnapshot(snapshot->domain, snap->def->parent);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return parent;
}

static virDomainSnapshotPtr
vzDomainSnapshotCurrent(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom;
    virDomainSnapshotPtr snapshot = NULL;
    virDomainSnapshotObjListPtr snapshots = NULL;
    virDomainSnapshotObjPtr current;

    virCheckFlags(0, NULL);

    if (!(dom = vzDomObjFromDomain(domain)))
        return NULL;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(current = vzFindCurrentSnapshot(snapshots))) {
        virReportError(VIR_ERR_NO_DOMAIN_SNAPSHOT, "%s",
                       _("the domain does not have a current snapshot"));
        goto cleanup;
    }

    snapshot = virGetDomainSnapshot(domain, current->def->name);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return snapshot;
}

static int
vzDomainSnapshotIsCurrent(virDomainSnapshotPtr snapshot, unsigned int flags)
{
    virDomainObjPtr dom;
    int ret = -1;
    virDomainSnapshotObjListPtr snapshots = NULL;
    virDomainSnapshotObjPtr current;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    current = vzFindCurrentSnapshot(snapshots);
    ret = current && STREQ(snapshot->name, current->def->name);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);

    return ret;
}

static int
vzDomainSnapshotHasMetadata(virDomainSnapshotPtr snapshot,
                              unsigned int flags)
{
    virDomainObjPtr dom;
    int ret = -1;
    virDomainSnapshotObjPtr snap;
    virDomainSnapshotObjListPtr snapshots = NULL;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(snap = vzSnapObjFromSnapshot(snapshots, snapshot)))
        goto cleanup;

    ret = 1;

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);
    return ret;
}

static virDomainSnapshotPtr
vzDomainSnapshotCreateXML(virDomainPtr domain,
                          const char *xmlDesc,
                          unsigned int flags)
{
    virDomainSnapshotDefPtr def = NULL;
    virDomainSnapshotPtr snapshot = NULL;
    virDomainObjPtr dom;
    vzConnPtr privconn = domain->conn->privateData;
    vzDriverPtr driver = privconn->driver;
    unsigned int parse_flags = VIR_DOMAIN_SNAPSHOT_PARSE_DISKS;
    virDomainSnapshotObjListPtr snapshots = NULL;
    virDomainSnapshotObjPtr current;

    virCheckFlags(0, NULL);

    if (!(dom = vzDomObjFromDomain(domain)))
        return NULL;

    if (!(def = virDomainSnapshotDefParseString(xmlDesc, driver->caps,
                                                driver->xmlopt, parse_flags)))
        goto cleanup;

    if (def->ndisks > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("configuring disks is not supported for vz snapshots"));
        goto cleanup;
    }

    if (def->memory) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("configuring memory location is not supported"));
        goto cleanup;
    }

    /* snaphot name is ignored, it will be set to auto generated by sdk uuid */
    if (prlsdkCreateSnapshot(dom, def->description) < 0)
        goto cleanup;

    if (!(snapshots = prlsdkLoadSnapshots(dom)))
        goto cleanup;

    if (!(current = vzFindCurrentSnapshot(snapshots))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("can't find created snapshot"));
        goto cleanup;
    }

    /* hopefully new current snapshot is newly created one */
    snapshot = virGetDomainSnapshot(domain, current->def->name);

 cleanup:
    virDomainSnapshotObjListFree(snapshots);
    virObjectUnlock(dom);
    virDomainSnapshotDefFree(def);

    return snapshot;
}

static int
vzDomainSnapshotDelete(virDomainSnapshotPtr snapshot, unsigned int flags)
{
    virDomainObjPtr dom;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    ret = prlsdkDeleteSnapshot(dom, snapshot->name,
                               flags & VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN);

    virObjectUnlock(dom);

    return ret;
}

static int
vzDomainRevertToSnapshot(virDomainSnapshotPtr snapshot, unsigned int flags)
{
    virDomainObjPtr dom;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED, -1);

    if (!(dom = vzDomObjFromDomain(snapshot->domain)))
        return -1;

    ret = prlsdkSwitchToSnapshot(dom, snapshot->name,
                                 flags & VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED);

    virObjectUnlock(dom);

    return ret;
}

typedef struct _vzMigrationCookie vzMigrationCookie;
typedef vzMigrationCookie *vzMigrationCookiePtr;
struct _vzMigrationCookie {
    unsigned char session_uuid[VIR_UUID_BUFLEN];
    unsigned char uuid[VIR_UUID_BUFLEN];
    char *name;
};

static void
vzMigrationCookieFree(vzMigrationCookiePtr mig)
{
    if (!mig)
        return;
    VIR_FREE(mig->name);
    VIR_FREE(mig);
}

static int
vzBakeCookie(vzMigrationCookiePtr mig, char **cookieout, int *cookieoutlen)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!cookieout || !cookieoutlen) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration cookie parameters are not provided."));
        return -1;
    }

    *cookieout = NULL;
    *cookieoutlen = 0;

    virBufferAddLit(&buf, "<vz-migration>\n");
    virBufferAdjustIndent(&buf, 2);
    virUUIDFormat(mig->session_uuid, uuidstr);
    virBufferAsprintf(&buf, "<session-uuid>%s</session-uuid>\n", uuidstr);
    virUUIDFormat(mig->uuid, uuidstr);
    virBufferAsprintf(&buf, "<uuid>%s</uuid>\n", uuidstr);
    virBufferAsprintf(&buf, "<name>%s</name>\n", mig->name);
    virBufferAdjustIndent(&buf, -2);
    virBufferAddLit(&buf, "</vz-migration>\n");

    if (virBufferCheckError(&buf) < 0)
        return -1;

    *cookieout = virBufferContentAndReset(&buf);
    *cookieoutlen = strlen(*cookieout) + 1;

    return 0;
}

static vzMigrationCookiePtr
vzEatCookie(const char *cookiein, int cookieinlen)
{
    xmlDocPtr doc = NULL;
    xmlXPathContextPtr ctx = NULL;
    char *tmp;
    vzMigrationCookiePtr mig = NULL;

    if (VIR_ALLOC(mig) < 0)
        return NULL;

    if (!cookiein || cookieinlen <= 0 || cookiein[cookieinlen - 1] != '\0') {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Invalid migration cookie"));
        goto error;
    }

    if (!(doc = virXMLParseStringCtxt(cookiein,
                                      _("(_migration_cookie)"), &ctx)))
        goto error;

    if (!(tmp = virXPathString("string(./session-uuid[1])", ctx))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing session-uuid element in migration data"));
        goto error;
    }
    if (virUUIDParse(tmp, mig->session_uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("malformed session-uuid element in migration data"));
        VIR_FREE(tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if (!(tmp = virXPathString("string(./uuid[1])", ctx))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing uuid element in migration data"));
        goto error;
    }
    if (virUUIDParse(tmp, mig->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("malformed uuid element in migration data"));
        VIR_FREE(tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if (!(mig->name = virXPathString("string(./name[1])", ctx))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing name element in migration data"));
        goto error;
    }

 cleanup:
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    return mig;

 error:
    vzMigrationCookieFree(mig);
    mig = NULL;
    goto cleanup;
}

#define VZ_MIGRATION_FLAGS         (VIR_MIGRATE_PAUSED |        \
                                    VIR_MIGRATE_PEER2PEER)

#define VZ_MIGRATION_PARAMETERS                                 \
    VIR_MIGRATE_PARAM_DEST_XML,         VIR_TYPED_PARAM_STRING, \
    VIR_MIGRATE_PARAM_URI,              VIR_TYPED_PARAM_STRING, \
    VIR_MIGRATE_PARAM_DEST_NAME,        VIR_TYPED_PARAM_STRING, \
    NULL

static char *
vzDomainMigrateBegin3Params(virDomainPtr domain,
                            virTypedParameterPtr params,
                            int nparams,
                            char **cookieout,
                            int *cookieoutlen,
                            unsigned int flags)
{
    char *xml = NULL;
    virDomainObjPtr dom = NULL;
    vzConnPtr privconn = domain->conn->privateData;
    vzMigrationCookiePtr mig = NULL;

    virCheckFlags(VZ_MIGRATION_FLAGS, NULL);

    if (virTypedParamsValidate(params, nparams, VZ_MIGRATION_PARAMETERS) < 0)
        goto cleanup;

    /* we can't do this check via VZ_MIGRATION_PARAMETERS as on preparation
     * step domain xml will be passed via this parameter and it is a common
     * style to use single allowed parameter list definition in all steps */
    if (virTypedParamsGet(params, nparams, VIR_MIGRATE_PARAM_DEST_XML)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("Changing destination XML is not supported"));
        goto cleanup;
    }

    if (!(dom = vzDomObjFromDomain(domain)))
        goto cleanup;

    if (VIR_ALLOC(mig) < 0)
        goto cleanup;

    if (VIR_STRDUP(mig->name, dom->def->name) < 0)
        goto cleanup;

    memcpy(mig->uuid, dom->def->uuid, VIR_UUID_BUFLEN);

    if (vzBakeCookie(mig, cookieout, cookieoutlen) < 0)
        goto cleanup;

    xml = virDomainDefFormat(dom->def, privconn->driver->caps,
                             VIR_DOMAIN_XML_MIGRATABLE);

 cleanup:

    vzMigrationCookieFree(mig);
    if (dom)
        virObjectUnlock(dom);
    return xml;
}

static char*
vzMigrationCreateURI(void)
{
    char *hostname = NULL;
    char *uri = NULL;

    if (!(hostname = virGetHostname()))
        goto cleanup;

    if (STRPREFIX(hostname, "localhost")) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("hostname on destination resolved to localhost,"
                         " but migration requires an FQDN"));
        goto cleanup;
    }

    if (virAsprintf(&uri, "vzmigr://%s", hostname) < 0)
        goto cleanup;

 cleanup:
    VIR_FREE(hostname);
    return uri;
}

static int
vzDomainMigratePrepare3Params(virConnectPtr conn,
                              virTypedParameterPtr params,
                              int nparams,
                              const char *cookiein,
                              int cookieinlen,
                              char **cookieout,
                              int *cookieoutlen,
                              char **uri_out,
                              unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    const char *miguri = NULL;
    const char *dname = NULL;
    virDomainObjPtr dom = NULL;
    vzMigrationCookiePtr mig = NULL;
    int ret = -1;

    virCheckFlags(VZ_MIGRATION_FLAGS, -1);

    if (virTypedParamsValidate(params, nparams, VZ_MIGRATION_PARAMETERS) < 0)
        goto cleanup;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_URI, &miguri) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME, &dname) < 0)
        goto cleanup;

    /* We must set uri_out if miguri is not set. This is direct
     * managed migration requirement */
    if (!miguri && !(*uri_out = vzMigrationCreateURI()))
        goto cleanup;

    if (!(mig = vzEatCookie(cookiein, cookieinlen)))
        goto cleanup;

    memcpy(mig->session_uuid, privconn->driver->session_uuid, VIR_UUID_BUFLEN);

    if (vzBakeCookie(mig, cookieout, cookieoutlen) < 0)
        goto cleanup;

    virObjectLock(privconn->driver);
    dom = virDomainObjListFindByUUID(privconn->driver->domains, mig->uuid);
    if (dom) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(mig->uuid, uuidstr);
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("A domain with uuid '%s' already exists"),
                       uuidstr);
        goto unlock;
    }

    if (!(dom = vzNewDomain(privconn->driver,
                            dname ? dname : mig->name, mig->uuid)))
        goto unlock;

    ret = 0;

 unlock:
    virObjectUnlock(privconn->driver);

 cleanup:
    vzMigrationCookieFree(mig);
    if (dom)
        virObjectUnlock(dom);
    return ret;
}

static int
vzConnectSupportsFeature(virConnectPtr conn ATTRIBUTE_UNUSED, int feature)
{
    switch (feature) {
    case VIR_DRV_FEATURE_MIGRATION_PARAMS:
    case VIR_DRV_FEATURE_MIGRATION_P2P:
        return 1;
    default:
        return 0;
    }
}

static virURIPtr
vzParseVzURI(const char *uri_str)
{
    virURIPtr uri = NULL;

    if (!(uri = virURIParse(uri_str)))
        goto error;

    if (!uri->scheme || !uri->server) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("scheme and host are mandatory vz migration URI: %s"),
                       uri_str);
        goto error;
    }

    if (uri->user || uri->path || uri->query || uri->fragment) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("only scheme, host and port are supported in "
                         "vz migration URI: %s"), uri_str);
        goto error;
    }

    if (STRNEQ(uri->scheme, "vzmigr")) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                       _("unsupported scheme %s in migration URI %s"),
                       uri->scheme, uri_str);
        goto error;
    }

    return uri;

 error:
    virURIFree(uri);
    return NULL;
}

static int
vzDomainMigratePerformStep(virDomainPtr domain,
                           virTypedParameterPtr params,
                           int nparams,
                           const char *cookiein,
                           int cookieinlen,
                           unsigned int flags)
{
    int ret = -1;
    virDomainObjPtr dom = NULL;
    virURIPtr vzuri = NULL;
    vzConnPtr privconn = domain->conn->privateData;
    const char *miguri = NULL;
    const char *dname = NULL;
    vzMigrationCookiePtr mig = NULL;

    virCheckFlags(VZ_MIGRATION_FLAGS, -1);

    if (virTypedParamsValidate(params, nparams, VZ_MIGRATION_PARAMETERS) < 0)
        goto cleanup;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_URI, &miguri) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME, &dname) < 0)
        goto cleanup;

    if (!miguri) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("migrate uri is not set"));
        goto cleanup;
    }

    if (!(mig = vzEatCookie(cookiein, cookieinlen)))
        goto cleanup;

    if (!(dom = vzDomObjFromDomain(domain)))
        goto cleanup;

    if (!(vzuri = vzParseVzURI(miguri)))
        goto cleanup;

    if (prlsdkMigrate(dom, vzuri, mig->session_uuid, dname, flags) < 0)
        goto cleanup;

    virDomainObjListRemove(privconn->driver->domains, dom);
    dom = NULL;

    ret = 0;

 cleanup:
    if (dom)
        virObjectUnlock(dom);
    virURIFree(vzuri);
    vzMigrationCookieFree(mig);

    return ret;
}

static int
vzDomainMigratePerformP2P(virDomainPtr domain,
                          const char *dconnuri,
                          virTypedParameterPtr orig_params,
                          int nparams,
                          unsigned int flags)
{
    virDomainPtr ddomain = NULL;
    char *uri = NULL;
    char *cookiein = NULL;
    char *cookieout = NULL;
    char *dom_xml = NULL;
    int cookieinlen = 0;
    int cookieoutlen = 0;
    virErrorPtr orig_err = NULL;
    int cancelled = 1;
    virConnectPtr dconn = NULL;
    virTypedParameterPtr params = NULL;
    int ret = -1;

    if (virTypedParamsCopy(&params, orig_params, nparams) < 0)
        return -1;

    if (!(dconn = virConnectOpen(dconnuri)))
        goto done;

    if (!(dom_xml = vzDomainMigrateBegin3Params(domain, params, nparams,
                                                &cookieout, &cookieoutlen,
                                                flags)))
        goto done;

    cookiein = cookieout;
    cookieinlen = cookieoutlen;
    cookieout = NULL;
    cookieoutlen = 0;
    if (dconn->driver->domainMigratePrepare3Params
            (dconn, params, nparams, cookiein, cookieinlen,
             &cookieout, &cookieoutlen, &uri, flags) < 0)
        goto done;

    /* preparation step was successful, thus on any error we must perform
     * finish step to finalize migration on target
     */
    if (uri && virTypedParamsReplaceString(&params, &nparams,
                                           VIR_MIGRATE_PARAM_URI, uri) < 0) {
        orig_err = virSaveLastError();
        goto finish;
    }

    VIR_FREE(cookiein);
    cookiein = cookieout;
    cookieinlen = cookieoutlen;
    cookieout = NULL;
    cookieoutlen = 0;
    if (vzDomainMigratePerformStep(domain, params, nparams, cookiein,
                                   cookieinlen, flags) < 0) {
        orig_err = virSaveLastError();
        goto finish;
    }

    cancelled = 0;

 finish:
    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME, NULL) <= 0 &&
        virTypedParamsReplaceString(&params, &nparams,
                                    VIR_MIGRATE_PARAM_DEST_NAME,
                                    domain->name) < 0)
        goto done;

    ddomain = dconn->driver->domainMigrateFinish3Params(dconn, params, nparams,
                                                        NULL, 0, NULL, NULL,
                                                        flags, cancelled);
    if (ddomain)
        ret = 0;
    virObjectUnref(ddomain);

    /* confirm step is NOOP thus no need to call it */

 done:
    if (orig_err) {
        virSetError(orig_err);
        virFreeError(orig_err);
    }
    VIR_FREE(dom_xml);
    VIR_FREE(uri);
    VIR_FREE(cookiein);
    VIR_FREE(cookieout);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dconn);
    return ret;
}

static int
vzDomainMigratePerform3Params(virDomainPtr domain,
                              const char *dconnuri,
                              virTypedParameterPtr params,
                              int nparams,
                              const char *cookiein,
                              int cookieinlen,
                              char **cookieout ATTRIBUTE_UNUSED,
                              int *cookieoutlen ATTRIBUTE_UNUSED,
                              unsigned int flags)
{
    virCheckFlags(VZ_MIGRATION_FLAGS, -1);

    if (virTypedParamsValidate(params, nparams, VZ_MIGRATION_PARAMETERS) < 0)
        return -1;

    if (flags & VIR_MIGRATE_PEER2PEER)
        return vzDomainMigratePerformP2P(domain, dconnuri, params, nparams, flags);
    else
        return vzDomainMigratePerformStep(domain, params, nparams,
                                          cookiein, cookieinlen, flags);

}

static virDomainPtr
vzDomainMigrateFinish3Params(virConnectPtr dconn,
                             virTypedParameterPtr params,
                             int nparams,
                             const char *cookiein ATTRIBUTE_UNUSED,
                             int cookieinlen ATTRIBUTE_UNUSED,
                             char **cookieout ATTRIBUTE_UNUSED,
                             int *cookieoutlen ATTRIBUTE_UNUSED,
                             unsigned int flags,
                             int cancelled)
{
    virDomainObjPtr dom = NULL;
    virDomainPtr domain = NULL;
    vzConnPtr privconn = dconn->privateData;
    vzDriverPtr driver = privconn->driver;
    const char *name = NULL;

    virCheckFlags(VZ_MIGRATION_FLAGS, NULL);

    if (virTypedParamsValidate(params, nparams, VZ_MIGRATION_PARAMETERS) < 0)
        goto cleanup;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME, &name) < 0)
        goto cleanup;

    if (!(dom = virDomainObjListFindByName(driver->domains, name))) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), name);
        goto cleanup;
    }

    if (cancelled) {
        virDomainObjListRemove(driver->domains, dom);
        dom = NULL;
        goto cleanup;
    }

    if (prlsdkLoadDomain(driver, dom))
        goto cleanup;

    domain = virGetDomain(dconn, dom->def->name, dom->def->uuid);
    if (domain)
        domain->id = dom->def->id;

 cleanup:
    /* In this situation we have to restore domain on source. But the migration
     * is already finished. */
    if (!cancelled && !domain)
        VIR_WARN("Can't provide domain '%s' after successfull migration.", name);
    virDomainObjEndAPI(&dom);
    return domain;
}

static int
vzDomainMigrateConfirm3Params(virDomainPtr domain ATTRIBUTE_UNUSED,
                              virTypedParameterPtr params,
                              int nparams,
                              const char *cookiein ATTRIBUTE_UNUSED,
                              int cookieinlen ATTRIBUTE_UNUSED,
                              unsigned int flags,
                              int cancelled ATTRIBUTE_UNUSED)
{
    virCheckFlags(VZ_MIGRATION_FLAGS, -1);

    if (virTypedParamsValidate(params, nparams, VZ_MIGRATION_PARAMETERS) < 0)
        return -1;

    return 0;
}

static virHypervisorDriver vzHypervisorDriver = {
    .name = "vz",
    .connectOpen = vzConnectOpen,            /* 0.10.0 */
    .connectClose = vzConnectClose,          /* 0.10.0 */
    .connectGetVersion = vzConnectGetVersion,   /* 0.10.0 */
    .connectGetHostname = vzConnectGetHostname,      /* 0.10.0 */
    .connectGetSysinfo = vzConnectGetSysinfo, /* 1.3.4 */
    .connectGetMaxVcpus = vzConnectGetMaxVcpus, /* 1.2.21 */
    .nodeGetInfo = vzNodeGetInfo,      /* 0.10.0 */
    .nodeGetCPUStats = vzNodeGetCPUStats,      /* 1.2.21 */
    .nodeGetMemoryStats = vzNodeGetMemoryStats, /* 1.2.21 */
    .nodeGetCellsFreeMemory = vzNodeGetCellsFreeMemory, /* 1.2.21 */
    .nodeGetFreeMemory = vzNodeGetFreeMemory, /* 1.2.21 */
    .connectGetCapabilities = vzConnectGetCapabilities,      /* 0.10.0 */
    .connectBaselineCPU = vzConnectBaselineCPU, /* 1.2.6 */
    .connectListDomains = vzConnectListDomains,      /* 0.10.0 */
    .connectNumOfDomains = vzConnectNumOfDomains,    /* 0.10.0 */
    .connectListDefinedDomains = vzConnectListDefinedDomains,        /* 0.10.0 */
    .connectNumOfDefinedDomains = vzConnectNumOfDefinedDomains,      /* 0.10.0 */
    .connectListAllDomains = vzConnectListAllDomains, /* 0.10.0 */
    .domainLookupByID = vzDomainLookupByID,    /* 0.10.0 */
    .domainLookupByUUID = vzDomainLookupByUUID,        /* 0.10.0 */
    .domainLookupByName = vzDomainLookupByName,        /* 0.10.0 */
    .domainGetOSType = vzDomainGetOSType,    /* 0.10.0 */
    .domainGetInfo = vzDomainGetInfo,  /* 0.10.0 */
    .domainGetState = vzDomainGetState,        /* 0.10.0 */
    .domainGetXMLDesc = vzDomainGetXMLDesc,    /* 0.10.0 */
    .domainIsPersistent = vzDomainIsPersistent,        /* 0.10.0 */
    .domainGetAutostart = vzDomainGetAutostart,        /* 0.10.0 */
    .domainGetVcpus = vzDomainGetVcpus, /* 1.2.6 */
    .domainSuspend = vzDomainSuspend,    /* 0.10.0 */
    .domainResume = vzDomainResume,    /* 0.10.0 */
    .domainDestroy = vzDomainDestroy,  /* 0.10.0 */
    .domainShutdown = vzDomainShutdown, /* 0.10.0 */
    .domainCreate = vzDomainCreate,    /* 0.10.0 */
    .domainCreateWithFlags = vzDomainCreateWithFlags, /* 1.2.10 */
    .domainReboot = vzDomainReboot, /* 1.3.0 */
    .domainDefineXML = vzDomainDefineXML,      /* 0.10.0 */
    .domainDefineXMLFlags = vzDomainDefineXMLFlags, /* 1.2.12 */
    .domainUndefine = vzDomainUndefine, /* 1.2.10 */
    .domainUndefineFlags = vzDomainUndefineFlags, /* 1.2.10 */
    .domainAttachDevice = vzDomainAttachDevice, /* 1.2.15 */
    .domainAttachDeviceFlags = vzDomainAttachDeviceFlags, /* 1.2.15 */
    .domainDetachDevice = vzDomainDetachDevice, /* 1.2.15 */
    .domainDetachDeviceFlags = vzDomainDetachDeviceFlags, /* 1.2.15 */
    .domainIsActive = vzDomainIsActive, /* 1.2.10 */
    .domainIsUpdated = vzDomainIsUpdated,     /* 1.2.21 */
    .domainGetVcpusFlags = vzDomainGetVcpusFlags, /* 1.2.21 */
    .domainGetMaxVcpus = vzDomainGetMaxVcpus, /* 1.2.21 */
    .connectDomainEventRegisterAny = vzConnectDomainEventRegisterAny, /* 1.2.10 */
    .connectDomainEventDeregisterAny = vzConnectDomainEventDeregisterAny, /* 1.2.10 */
    .nodeGetCPUMap = vzNodeGetCPUMap, /* 1.2.8 */
    .connectIsEncrypted = vzConnectIsEncrypted, /* 1.2.5 */
    .connectIsSecure = vzConnectIsSecure, /* 1.2.5 */
    .connectIsAlive = vzConnectIsAlive, /* 1.2.5 */
    .domainHasManagedSaveImage = vzDomainHasManagedSaveImage, /* 1.2.13 */
    .domainManagedSave = vzDomainManagedSave, /* 1.2.14 */
    .domainManagedSaveRemove = vzDomainManagedSaveRemove, /* 1.2.14 */
    .domainGetMaxMemory = vzDomainGetMaxMemory, /* 1.2.15 */
    .domainBlockStats = vzDomainBlockStats, /* 1.2.17 */
    .domainBlockStatsFlags = vzDomainBlockStatsFlags, /* 1.2.17 */
    .domainInterfaceStats = vzDomainInterfaceStats, /* 1.2.17 */
    .domainMemoryStats = vzDomainMemoryStats, /* 1.2.17 */
    .connectRegisterCloseCallback = vzConnectRegisterCloseCallback, /* 1.3.2 */
    .connectUnregisterCloseCallback = vzConnectUnregisterCloseCallback, /* 1.3.2 */
    .domainSetMemoryFlags = vzDomainSetMemoryFlags, /* 1.3.4 */
    .domainSetMemory = vzDomainSetMemory, /* 1.3.4 */
    .domainSnapshotNum = vzDomainSnapshotNum, /* 1.3.5 */
    .domainSnapshotListNames = vzDomainSnapshotListNames, /* 1.3.5 */
    .domainListAllSnapshots = vzDomainListAllSnapshots, /* 1.3.5 */
    .domainSnapshotGetXMLDesc = vzDomainSnapshotGetXMLDesc, /* 1.3.5 */
    .domainSnapshotNumChildren = vzDomainSnapshotNumChildren, /* 1.3.5 */
    .domainSnapshotListChildrenNames = vzDomainSnapshotListChildrenNames, /* 1.3.5 */
    .domainSnapshotListAllChildren = vzDomainSnapshotListAllChildren, /* 1.3.5 */
    .domainSnapshotLookupByName = vzDomainSnapshotLookupByName, /* 1.3.5 */
    .domainHasCurrentSnapshot = vzDomainHasCurrentSnapshot, /* 1.3.5 */
    .domainSnapshotGetParent = vzDomainSnapshotGetParent, /* 1.3.5 */
    .domainSnapshotCurrent = vzDomainSnapshotCurrent, /* 1.3.5 */
    .domainSnapshotIsCurrent = vzDomainSnapshotIsCurrent, /* 1.3.5 */
    .domainSnapshotHasMetadata = vzDomainSnapshotHasMetadata, /* 1.3.5 */
    .domainSnapshotCreateXML = vzDomainSnapshotCreateXML, /* 1.3.5 */
    .domainSnapshotDelete = vzDomainSnapshotDelete, /* 1.3.5 */
    .domainRevertToSnapshot = vzDomainRevertToSnapshot, /* 1.3.5 */
    .connectSupportsFeature = vzConnectSupportsFeature, /* 1.3.5 */
    .domainMigrateBegin3Params = vzDomainMigrateBegin3Params, /* 1.3.5 */
    .domainMigratePrepare3Params = vzDomainMigratePrepare3Params, /* 1.3.5 */
    .domainMigratePerform3Params = vzDomainMigratePerform3Params, /* 1.3.5 */
    .domainMigrateFinish3Params = vzDomainMigrateFinish3Params, /* 1.3.5 */
    .domainMigrateConfirm3Params = vzDomainMigrateConfirm3Params, /* 1.3.5 */
};

static virConnectDriver vzConnectDriver = {
    .hypervisorDriver = &vzHypervisorDriver,
};

static int
vzStateCleanup(void)
{
    virObjectUnref(vz_driver);
    vz_driver = NULL;
    virMutexDestroy(&vz_driver_lock);
    prlsdkDeinit();
    return 0;
}

static int
vzStateInitialize(bool privileged ATTRIBUTE_UNUSED,
                  virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                  void *opaque ATTRIBUTE_UNUSED)
{
    if (prlsdkInit() < 0) {
        VIR_DEBUG("%s", _("Can't initialize Parallels SDK"));
        return -1;
    }

   if (virMutexInit(&vz_driver_lock) < 0)
        goto error;

    /* Failing to create driver here is not fatal and only means
     * that next driver client will try once more when connecting */
    vz_driver = vzDriverObjNew();
    return 0;

 error:
    vzStateCleanup();
    return -1;
}

static virStateDriver vzStateDriver = {
    .name = "vz",
    .stateInitialize = vzStateInitialize,
    .stateCleanup = vzStateCleanup,
};

/* Parallels domain type backward compatibility*/
static virHypervisorDriver parallelsHypervisorDriver;
static virConnectDriver parallelsConnectDriver;

/**
 * vzRegister:
 *
 * Registers the vz driver
 */
int
vzRegister(void)
{
    char *prlctl_path;

    prlctl_path = virFindFileInPath(PRLCTL);
    if (!prlctl_path) {
        VIR_DEBUG("%s", _("Can't find prlctl command in the PATH env"));
        return 0;
    }

    VIR_FREE(prlctl_path);

    /* Backward compatibility with Parallels domain type */
    parallelsHypervisorDriver = vzHypervisorDriver;
    parallelsHypervisorDriver.name = "Parallels";
    parallelsConnectDriver = vzConnectDriver;
    parallelsConnectDriver.hypervisorDriver = &parallelsHypervisorDriver;
    if (virRegisterConnectDriver(&parallelsConnectDriver, true) < 0)
        return -1;

    if (virRegisterConnectDriver(&vzConnectDriver, true) < 0)
        return -1;

    if (virRegisterStateDriver(&vzStateDriver) < 0)
        return -1;

    return 0;
}
