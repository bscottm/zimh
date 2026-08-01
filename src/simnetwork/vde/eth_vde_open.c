#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_ether.h"
#include "sim_ether_internal.h"
#include "simnetwork/eth_backends.h"

#include <libvdeplug.h>

t_stat eth_vde_open(const char *devname, ETH_DEV *dev, char *savname, size_t savname_size)
{
    char vdeswitch_s[CBUFSIZE]; /* VDE switch name */
    char vdeport_s[CBUFSIZE];   /* VDE switch port (optional), numeric */

    struct vde_open_args voa;
    const char *devname = savname + 4;

    memset(&voa, 0, sizeof(voa));
    voa.mode = 0600;
    if (!strcmp(savname, "vde:vdedevice"))
        return sim_messagef(SCPE_OPENERR, "Eth: Must specify actual vde device name (i.e. vde:/tmp/switch)\n");
    while (isspace(*devname))
        ++devname;
    devname = get_glyph_nc(devname, vdeswitch_s, ':'); /* Extract switch name          */
    devname = get_glyph_nc(devname, vdeport_s, 0);     /* Extract optional port number */

    if (vdeport_s[0]) {                                /* port provided? */
        t_stat r;

        voa.port = (int)get_uint(vdeport_s, 10, 255, &r);
        if (r != SCPE_OK)
            return sim_messagef(SCPE_OPENERR, "Eth: Invalid vde port number: %s in %s\n", vdeport_s, savname);
    }

    VDECONN *vde;
    if ((vde = vde_open((char *)vdeswitch_s, (char *)"simh", &voa)) == NULL)
        return sim_messagef(SCPE_OPENERR, "Eth: Failed to open VDE device %s: %s\n", savname, strerror(errno));

    eth_backend_t *backend;

    if ((backend = (eth_backend_t *)calloc(1, sizeof(*backend))) == NULL) {
        vde_close(vde);
        return sim_messagef(SCPE_MEM, "Eth: Failed to allocate memory for VDE backend %s\n", savname);
    }

    backend->eth_api = ETH_API_VDE;
    backend->packet_wait = eth_wait_vde;
    backend->packet_read = eth_reader_vde;
    backend->before_packet_write = NULL;
    backend->write_packet = eth_writer_vde;
    backend->after_packet_write = NULL;
    backend->state.vde = vde;

    eth_dev->backend = backend;

    return SCPE_OK;
}