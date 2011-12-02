/* 
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file "COPYING" in the main directory of
 * this archive for more details.
 *
 * Copyright (C) 2005 by Christian Limpach
 * Changelog:
 *  Dec 1. 2011. add multiple fds (Yang Jian)
 */

#include <err.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <xenctrl.h>
#include <xenguest.h>

int
main(int argc, char **argv)
{
    unsigned int domid, store_evtchn, console_evtchn;
    unsigned int hvm, pae, apic, io_fd_num;
    xc_interface *xch;
    int ret, *io_fd, i_fd;
    int superpages;
    unsigned long store_mfn, console_mfn;

    if ( (argc != 8) && (argc != 9) )
        errx(1, "usage: %s iofd domid store_evtchn "
             "console_evtchn hvm pae apic [superpages]", argv[0]);

    xch = xc_interface_open(0,0,0);
    if ( !xch )
        errx(1, "failed to open control interface");

    io_fd_num = atoi(argv[1]);
    io_fd = malloc(sizeof(int) * io_fd_num);
    for (i_fd = 0; i_fd < io_fd_num; i_fd++){
        io_fd[i_fd] = atoi(argv[2 + i_fd]);
    }
    domid = atoi(argv[2 + i_fd]);
    store_evtchn = atoi(argv[3 + i_fd]);
    console_evtchn = atoi(argv[4 + i_fd]);
    hvm  = atoi(argv[5 + i_fd]);
    pae  = atoi(argv[6 + i_fd]);
    apic = atoi(argv[7 + i_fd]);
    if ( argc == 9 + i_fd )
	    superpages = atoi(argv[8 + i_fd]);
    else
	    superpages = 0;

    ret = xc_domain_restore(xch, io_fd_num, io_fd, domid, store_evtchn, &store_mfn,
                            console_evtchn, &console_mfn, hvm, pae, superpages);

    if ( ret == 0 )
    {
	printf("store-mfn %li\n", store_mfn);
        if ( !hvm )
            printf("console-mfn %li\n", console_mfn);
	fflush(stdout);
    }

    xc_interface_close(xch);

    return ret;
}
