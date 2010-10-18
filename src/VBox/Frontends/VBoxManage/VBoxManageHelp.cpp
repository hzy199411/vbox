/* $Id$ */
/** @file
 * VBoxManage - help and other message output.
 */

/*
 * Copyright (C) 2006-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <VBox/version.h>

#include <iprt/ctype.h>
#include <iprt/err.h>
#include <iprt/getopt.h>
#include <iprt/stream.h>

#include "VBoxManage.h"



void showLogo(PRTSTREAM pStrm)
{
    static bool s_fShown; /* show only once */

    if (!s_fShown)
    {
        RTStrmPrintf(pStrm, VBOX_PRODUCT " Command Line Management Interface Version "
                     VBOX_VERSION_STRING "\n"
                     "(C) 2005-" VBOX_C_YEAR " " VBOX_VENDOR "\n"
                     "All rights reserved.\n"
                     "\n");
        s_fShown = true;
    }
}

void printUsage(USAGECATEGORY u64Cmd, PRTSTREAM pStrm)
{
    bool fDumpOpts = false;
#ifdef RT_OS_LINUX
    bool fLinux = true;
#else
    bool fLinux = false;
#endif
#ifdef RT_OS_WINDOWS
    bool fWin = true;
#else
    bool fWin = false;
#endif
#ifdef RT_OS_SOLARIS
    bool fSolaris = true;
#else
    bool fSolaris = false;
#endif
#ifdef RT_OS_FREEBSD
    bool fFreeBSD = true;
#else
    bool fFreeBSD = false;
#endif
#ifdef RT_OS_DARWIN
    bool fDarwin = true;
#else
    bool fDarwin = false;
#endif
#ifdef VBOX_WITH_VRDP
    bool fVRDP = true;
#else
    bool fVRDP = false;
#endif
#ifdef VBOX_WITH_VBOXSDL
    bool fVBoxSDL = true;
#else
    bool fVBoxSDL = false;
#endif

    if (u64Cmd == USAGE_DUMPOPTS)
    {
        fDumpOpts = true;
        fLinux = true;
        fWin = true;
        fSolaris = true;
        fFreeBSD = true;
        fDarwin = true;
        fVRDP = true;
        fVBoxSDL = true;
        u64Cmd = USAGE_ALL;
    }

    RTStrmPrintf(pStrm,
                 "Usage:\n"
                 "\n");

    if (u64Cmd == USAGE_ALL)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage [-v|--version]    print version number and exit\n"
                     "VBoxManage [-q|--nologo] ... suppress the logo\n"
                     "\n");
    }

    if (u64Cmd & USAGE_LIST)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage list [--long|-l] vms|runningvms|ostypes|hostdvds|hostfloppies|\n"
#if defined(VBOX_WITH_NETFLT)
                     "                            bridgedifs|hostonlyifs|dhcpservers|hostinfo|\n"
#else
                     "                            bridgedifs|dhcpservers|hostinfo|\n"
#endif
                     "                            hostcpuids|hddbackends|hdds|dvds|floppies|\n"
                     "                            usbhost|usbfilters|systemproperties\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SHOWVMINFO)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage showvminfo       <uuid>|<name> [--details] [--statistics]\n"
                     "                            [--machinereadable]\n"
                     "VBoxManage showvminfo       <uuid>|<name> --log <idx>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_REGISTERVM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage registervm       <filename>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_UNREGISTERVM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage unregistervm     <uuid>|<name> [--delete]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_CREATEVM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage createvm         --name <name>\n"
                     "                            [--ostype <ostype>]\n"
                     "                            [--register]\n"
                     "                            [--basefolder <path> | --settingsfile <path>]\n"
                     "                            [--uuid <uuid>]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_MODIFYVM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage modifyvm         <uuid|name>\n"
                     "                            [--name <name>]\n"
                     "                            [--ostype <ostype>]\n"
                     "                            [--memory <memorysize in MB>]\n"
                     "                            [--pagefusion on|off]\n"
                     "                            [--vram <vramsize in MB>]\n"
                     "                            [--acpi on|off]\n"
                     "                            [--ioapic on|off]\n"
                     "                            [--pae on|off]\n"
                     "                            [--hpet on|off]\n"
                     "                            [--hwvirtex on|off]\n"
                     "                            [--hwvirtexexcl on|off]\n"
                     "                            [--nestedpaging on|off]\n"
                     "                            [--largepages on|off]\n"
                     "                            [--vtxvpid on|off]\n"
                     "                            [--synthcpu on|off]\n"
                     "                            [--cpuidset <leaf> <eax> <ebx> <ecx> <edx>]\n"
                     "                            [--cpuidremove <leaf>]\n"
                     "                            [--cpuidremoveall]\n"
                     "                            [--hardwareuuid <uuid>]\n"
                     "                            [--cpus <number>]\n"
                     "                            [--cpuhotplug on|off]\n"
                     "                            [--plugcpu <id>]\n"
                     "                            [--unplugcpu <id>]\n"
                     "                            [--cpuexecutioncap <1-100>]\n"
                     "                            [--rtcuseutc on|off]\n"
                     "                            [--monitorcount <number>]\n"
                     "                            [--accelerate3d on|off]\n"
#ifdef VBOX_WITH_VIDEOHWACCEL
                     "                            [--accelerate2dvideo on|off]\n"
#endif
                     "                            [--firmware bios|efi|efi32|efi64]\n"
                     "                            [--chipset ich9|piix3]\n"
                     "                            [--bioslogofadein on|off]\n"
                     "                            [--bioslogofadeout on|off]\n"
                     "                            [--bioslogodisplaytime <msec>]\n"
                     "                            [--bioslogoimagepath <imagepath>]\n"
                     "                            [--biosbootmenu disabled|menuonly|messageandmenu]\n"
                     "                            [--biossystemtimeoffset <msec>]\n"
                     "                            [--biospxedebug on|off]\n"
                     "                            [--boot<1-4> none|floppy|dvd|disk|net>]\n"
                     "                            [--nic<1-N> none|null|nat|bridged|intnet"
#if defined(VBOX_WITH_NETFLT)
                     "|hostonly"
#endif
#ifdef VBOX_WITH_VDE
                     "|\n"
                     "                                        vde"
#endif
                     "]\n"
                     "                            [--nictype<1-N> Am79C970A|Am79C973"
#ifdef VBOX_WITH_E1000
                  "|\n                                            82540EM|82543GC|82545EM"
#endif
#ifdef VBOX_WITH_VIRTIO
                  "|\n                                            virtio"
#endif /* VBOX_WITH_VIRTIO */
                     "]\n"
                     "                            [--cableconnected<1-N> on|off]\n"
                     "                            [--nictrace<1-N> on|off]\n"
                     "                            [--nictracefile<1-N> <filename>]\n"
                     "                            [--nicspeed<1-N> <kbps>]\n"
                     "                            [--nicbootprio<1-N> <priority>]\n"
                     "                            [--bridgeadapter<1-N> none|<devicename>]\n"
#if defined(VBOX_WITH_NETFLT)
                     "                            [--hostonlyadapter<1-N> none|<devicename>]\n"
#endif
                     "                            [--intnet<1-N> <network name>]\n"
                     "                            [--natnet<1-N> <network>|default]\n"
#ifdef VBOX_WITH_VDE
                     "                            [--vdenet<1-N> <network>|default]\n"
#endif
                     "                            [--natsettings<1-N> [<mtu>],[<socksnd>],\n"
                     "                                                [<sockrcv>],[<tcpsnd>],\n"
                     "                                                [<tcprcv>]]\n"
                     "                            [--natpf<1-N> [<rulename>],tcp|udp,[<hostip>],\n"
                     "                                          <hostport>,[<guestip>],<guestport>]\n"
                     "                            [--natpf<1-N> delete <rulename>]\n"
                     "                            [--nattftpprefix<1-N> <prefix>]\n"
                     "                            [--nattftpfile<1-N> <file>]\n"
                     "                            [--nattftpserver<1-N> <ip>]\n"
                     "                            [--natdnspassdomain<1-N> on|off]\n"
                     "                            [--natdnsproxy<1-N> on|off]\n"
                     "                            [--natdnshostresolver<1-N> on|off]\n"
                     "                            [--nataliasmode<1-N> default|[log],[proxyonly],\n"
                     "                                                         [sameports]]\n"
                     "                            [--macaddress<1-N> auto|<mac>]\n"
                     "                            [--mouse ps2|usb|usbtablet\n"
                     "                            [--keyboard ps2|usb\n"
                     "                            [--uart<1-N> off|<I/O base> <IRQ>]\n"
                     "                            [--uartmode<1-N> disconnected|\n"
                     "                                             server <pipe>|\n"
                     "                                             client <pipe>|\n"
                     "                                             file <file>|\n"
                     "                                             <devicename>]\n"
                     "                            [--guestmemoryballoon <balloonsize in MB>]\n"
                     "                            [--gueststatisticsinterval <seconds>]\n"
                     "                            [--audio none|null");
        if (fWin)
        {
#ifdef VBOX_WITH_WINMM
            RTStrmPrintf(pStrm, "|winmm|dsound");
#else
            RTStrmPrintf(pStrm, "|dsound");
#endif
        }
        if (fSolaris)
        {
            RTStrmPrintf(pStrm, "|solaudio"
#ifdef VBOX_WITH_SOLARIS_OSS
                                    "|oss"
#endif
                        );
        }
        if (fLinux)
        {
            RTStrmPrintf(pStrm, "|oss"
#ifdef VBOX_WITH_ALSA
                                    "|alsa"
#endif
#ifdef VBOX_WITH_PULSE
                                    "|pulse"
#endif
                        );
        }
        if (fFreeBSD)
        {
            /* Get the line break sorted when dumping all option variants. */
            if (fDumpOpts)
            {
                RTStrmPrintf(pStrm, "|\n"
                     "                                     oss");
            }
            else
                RTStrmPrintf(pStrm, "|oss");
#ifdef VBOX_WITH_PULSE
            RTStrmPrintf(pStrm, "|pulse");
#endif
        }
        if (fDarwin)
        {
            RTStrmPrintf(pStrm, "|coreaudio");
        }
        RTStrmPrintf(pStrm, "]\n");
        RTStrmPrintf(pStrm,
                     "                            [--audiocontroller ac97|hda|sb16]\n"
                     "                            [--clipboard disabled|hosttoguest|guesttohost|\n"
                     "                                         bidirectional]\n");
        if (fVRDP)
        {
            RTStrmPrintf(pStrm,
                     "                            [--vrdp on|off]\n"
                     "                            [--vrdpport default|<ports>]\n"
                     "                            [--vrdpaddress <host>]\n"
                     "                            [--vrdpauthtype null|external|guest]\n"
                     "                            [--vrdpmulticon on|off]\n"
                     "                            [--vrdpreusecon on|off]\n"
                     "                            [--vrdpvideochannel on|off]\n"
                     "                            [--vrdpvideochannelquality <percent>]\n");
        }
        RTStrmPrintf(pStrm,
                     "                            [--usb on|off]\n"
                     "                            [--usbehci on|off]\n"
                     "                            [--snapshotfolder default|<path>]\n"
                     "                            [--teleporter on|off]\n"
                     "                            [--teleporterport <port>]\n"
                     "                            [--teleporteraddress <address|empty>\n"
                     "                            [--teleporterpassword <password>]\n"
#if 0
                     "                            [--iocache on|off]\n"
                     "                            [--iocachesize <I/O cache size in MB>]\n"
#endif
#if 0
                     "                            [--faulttolerance master|standby]\n"
                     "                            [--faulttoleranceaddress <name>]\n"
                     "                            [--faulttoleranceport <port>]\n"
                     "                            [--faulttolerancesyncinterval <msec>]\n"
                     "                            [--faulttolerancepassword <password>]\n"
#endif
                     "\n");
    }

    if (u64Cmd & USAGE_IMPORTAPPLIANCE)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage import           <ovf> [--dry-run|-n] [more options]\n"
                     "                            (run with -n to have options displayed\n"
                     "                             for a particular OVF)\n\n");
    }

    if (u64Cmd & USAGE_EXPORTAPPLIANCE)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage export           <machines> --output|-o <ovf>\n"
                     "                            [--legacy09]\n"
                     "                            [--manifest]\n"
                     "                            [--vsys <number of virtual system>]\n"
                     "                                    [--product <product name>]\n"
                     "                                    [--producturl <product url>]\n"
                     "                                    [--vendor <vendor name>]\n"
                     "                                    [--vendorurl <vendor url>]\n"
                     "                                    [--version <version info>]\n"
                     "                                    [--eula <license text>]\n"
                     "                                    [--eulafile <filename>]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_STARTVM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage startvm          <uuid>|<name>\n");
        RTStrmPrintf(pStrm,
                     "                            [--type gui");
        if (fVBoxSDL)
            RTStrmPrintf(pStrm, "|sdl");
        if (fVRDP)
            RTStrmPrintf(pStrm, "|vrdp");
        RTStrmPrintf(pStrm, "|headless]\n");
        RTStrmPrintf(pStrm,
                     "\n");
    }

    if (u64Cmd & USAGE_CONTROLVM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage controlvm        <uuid>|<name>\n"
                     "                            pause|resume|reset|poweroff|savestate|\n"
                     "                            acpipowerbutton|acpisleepbutton|\n"
                     "                            keyboardputscancode <hex> [<hex> ...]|\n"
                     "                            injectnmi|\n"
                     "                            setlinkstate<1-N> on|off |\n"
#ifdef VBOX_DYNAMIC_NET_ATTACH
#if defined(VBOX_WITH_NETFLT)
                     "                            nic<1-N> null|nat|bridged|intnet|hostonly\n"
                     "                                     [<devicename>] |\n"
#else /* !RT_OS_LINUX && !RT_OS_DARWIN */
                     "                            nic<1-N> null|nat|bridged|intnet\n"
                     "                                     [<devicename>] |\n"
#endif /* !RT_OS_LINUX && !RT_OS_DARWIN  */
                     "                            nictrace<1-N> on|off\n"
                     "                            nictracefile<1-N> <filename>\n"
#endif /* VBOX_DYNAMIC_NET_ATTACH */
                     "                            guestmemoryballoon <balloonsize in MB>]\n"
                     "                            gueststatisticsinterval <seconds>]\n"
                     "                            usbattach <uuid>|<address> |\n"
                     "                            usbdetach <uuid>|<address> |\n");
        if (fVRDP)
        {
            RTStrmPrintf(pStrm,
                     "                            vrdp on|off |\n");
            RTStrmPrintf(pStrm,
                     "                            vrdpport default|<ports> |\n"
                     "                            vrdpvideochannelquality <percent>\n");
        }
        RTStrmPrintf(pStrm,
                     "                            setvideomodehint <xres> <yres> <bpp> [display] |\n"
                     "                            setcredentials <username> <password> <domain>\n"
                     "                                           [--allowlocallogon <yes|no>] |\n"
                     "                            teleport --host <name> --port <port>\n"
                     "                                   [--maxdowntime <msec>] [--password password]\n"
                     "                            plugcpu <id>\n"
                     "                            unplugcpu <id>\n"
                     "                            cpuexecutioncap <1-100>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_DISCARDSTATE)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage discardstate     <uuid>|<name>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_ADOPTSTATE)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage adoptstate       <uuid>|<name> <state_file>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SNAPSHOT)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage snapshot         <uuid>|<name>\n"
                     "                            take <name> [--description <desc>] [--pause] |\n"
                     "                            delete <uuid>|<name> |\n"
                     "                            restore <uuid>|<name> |\n"
                     "                            restorecurrent |\n"
                     "                            edit <uuid>|<name>|--current\n"
                     "                                 [--name <name>]\n"
                     "                                 [--description <desc>] |\n"
                     "                            showvminfo <uuid>|<name>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_OPENMEDIUM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage openmedium       disk|dvd|floppy <filename>\n"
                     "                            [--type normal|immutable|writethrough|\n"
                     "                                    shareable] (disk only)\n"
                     "                            [--uuid <uuid>]\n"
                     "                            [--parentuuid <uuid>] (disk only)\n"
                     "\n");
    }

    if (u64Cmd & USAGE_CLOSEMEDIUM)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage closemedium      disk|dvd|floppy <uuid>|<filename>\n"
                     "                            [--delete]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_STORAGEATTACH)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage storageattach    <uuid|vmname>\n"
                     "                            --storagectl <name>\n"
                     "                            --port <number>\n"
                     "                            --device <number>\n"
                     "                            [--type dvddrive|hdd|fdd]\n"
                     "                            [--medium none|emptydrive|\n"
                     "                                      <uuid>|<filename>|host:<drive>]\n"
                     "                            [--passthrough on|off]\n"
                     "                            [--forceunmount]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_STORAGECONTROLLER)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage storagectl       <uuid|vmname>\n"
                     "                            --name <name>\n"
                     "                            [--add ide|sata|scsi|floppy|sas]\n"
                     "                            [--controller LSILogic|LSILogicSAS|BusLogic|\n"
                     "                                          IntelAHCI|PIIX3|PIIX4|ICH6|I82078]\n"
                     "                            [--sataideemulation<1-4> <1-30>]\n"
                     "                            [--sataportcount <1-30>]\n"
                     "                            [--hostiocache on|off]\n"
                     "                            [--remove]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SHOWHDINFO)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage showhdinfo       <uuid>|<filename>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_CREATEHD)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage createhd         --filename <filename>\n"
                     "                            --size <megabytes>|--sizebyte <bytes>\n"
                     "                            [--format VDI|VMDK|VHD] (default: VDI)\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
                     "                            [--type normal|writethrough|\n"
                     "                                    shareable] (default: normal)\n"
                     "                            [--comment <comment>]\n"
                     "                            [--remember]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_MODIFYHD)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage modifyhd         <uuid>|<filename>\n"
                     "                            [--type normal|writethrough|immutable|shareable]\n"
                     "                            [--autoreset on|off]\n"
                     "                            [--compact]\n"
                     "                            [--resize <megabytes>]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_CLONEHD)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage clonehd          <uuid>|<filename> <outputfile>\n"
                     "                            [--format VDI|VMDK|VHD|RAW|<other>]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
                     "                            [--type normal|writethrough|immutable|shareable]\n"
                     "                            [--remember] [--existing]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_CONVERTFROMRAW)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage convertfromraw   <filename> <outputfile>\n"
                     "                            [--format VDI|VMDK|VHD]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
#ifndef RT_OS_WINDOWS
                     "VBoxManage convertfromraw   stdin <outputfile> <bytes>\n"
                     "                            [--format VDI|VMDK|VHD]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
#endif
                     "\n");
    }

    if (u64Cmd & USAGE_ADDISCSIDISK)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage addiscsidisk     --server <name>|<ip>\n"
                     "                            --target <target>\n"
                     "                            [--port <port>]\n"
                     "                            [--lun <lun>]\n"
                     "                            [--encodedlun <lun>]\n"
                     "                            [--username <username>]\n"
                     "                            [--password <password>]\n"
                     "                            [--type normal|writethrough|immutable|shareable]\n"
                     "                            [--intnet]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_GETEXTRADATA)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage getextradata     global|<uuid>|<name>\n"
                     "                            <key>|enumerate\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SETEXTRADATA)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage setextradata     global|<uuid>|<name>\n"
                     "                            <key>\n"
                     "                            [<value>] (no value deletes key)\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SETPROPERTY)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage setproperty      hdfolder default|<folder> |\n"
                     "                            machinefolder default|<folder> |\n"
                     "                            vrdpauthlibrary default|<library> |\n"
                     "                            websrvauthlibrary default|null|<library> |\n"
                     "                            loghistorycount <value>\n"
                     "\n");
    }

    if (u64Cmd & USAGE_USBFILTER_ADD)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage usbfilter        add <index,0-N>\n"
                     "                            --target <uuid>|<name>|global\n"
                     "                            --name <string>\n"
                     "                            --action ignore|hold (global filters only)\n"
                     "                            [--active yes|no] (yes)\n"
                     "                            [--vendorid <XXXX>] (null)\n"
                     "                            [--productid <XXXX>] (null)\n"
                     "                            [--revision <IIFF>] (null)\n"
                     "                            [--manufacturer <string>] (null)\n"
                     "                            [--product <string>] (null)\n"
                     "                            [--remote yes|no] (null, VM filters only)\n"
                     "                            [--serialnumber <string>] (null)\n"
                     "                            [--maskedinterfaces <XXXXXXXX>]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_USBFILTER_MODIFY)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage usbfilter        modify <index,0-N>\n"
                     "                            --target <uuid>|<name>|global\n"
                     "                            [--name <string>]\n"
                     "                            [--action ignore|hold] (global filters only)\n"
                     "                            [--active yes|no]\n"
                     "                            [--vendorid <XXXX>|\"\"]\n"
                     "                            [--productid <XXXX>|\"\"]\n"
                     "                            [--revision <IIFF>|\"\"]\n"
                     "                            [--manufacturer <string>|\"\"]\n"
                     "                            [--product <string>|\"\"]\n"
                     "                            [--remote yes|no] (null, VM filters only)\n"
                     "                            [--serialnumber <string>|\"\"]\n"
                     "                            [--maskedinterfaces <XXXXXXXX>]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_USBFILTER_REMOVE)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage usbfilter        remove <index,0-N>\n"
                     "                            --target <uuid>|<name>|global\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SHAREDFOLDER_ADD)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage sharedfolder     add <vmname>|<uuid>\n"
                     "                            --name <name> --hostpath <hostpath>\n"
                     "                            [--transient] [--readonly] [--automount]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_SHAREDFOLDER_REMOVE)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage sharedfolder     remove <vmname>|<uuid>\n"
                     "                            --name <name> [--transient]\n"
                     "\n");
    }

    if (u64Cmd & USAGE_VM_STATISTICS)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage vmstatistics     <vmname>|<uuid> [--reset]\n"
                     "                            [--pattern <pattern>] [--descriptions]\n"
                     "\n");
    }

#ifdef VBOX_WITH_GUEST_PROPS
    if (u64Cmd & USAGE_GUESTPROPERTY)
        usageGuestProperty(pStrm);
#endif /* VBOX_WITH_GUEST_PROPS defined */

#ifdef VBOX_WITH_GUEST_CONTROL
    if (u64Cmd & USAGE_GUESTCONTROL)
        usageGuestControl(pStrm);
#endif /* VBOX_WITH_GUEST_CONTROL defined */

    if (u64Cmd & USAGE_METRICS)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage metrics          list [*|host|<vmname> [<metric_list>]]\n"
                     "                                                 (comma-separated)\n\n"
                     "VBoxManage metrics          setup\n"
                     "                            [--period <seconds>] (default: 1)\n"
                     "                            [--samples <count>] (default: 1)\n"
                     "                            [--list]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n\n"
                     "VBoxManage metrics          query [*|host|<vmname> [<metric_list>]]\n\n"
                     "VBoxManage metrics          enable\n"
                     "                            [--list]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n\n"
                     "VBoxManage metrics          disable\n"
                     "                            [--list]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n\n"
                     "VBoxManage metrics          collect\n"
                     "                            [--period <seconds>] (default: 1)\n"
                     "                            [--samples <count>] (default: 1)\n"
                     "                            [--list]\n"
                     "                            [--detach]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n"
                     "\n");
    }
#if defined(VBOX_WITH_NETFLT)
    if (u64Cmd & USAGE_HOSTONLYIFS)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage hostonlyif       ipconfig <name>\n"
                     "                            [--dhcp |\n"
                     "                            --ip<ipv4> [--netmask<ipv4> (def: 255.255.255.0)] |\n"
                     "                            --ipv6<ipv6> [--netmasklengthv6<length> (def: 64)]]\n"
# if defined(RT_OS_WINDOWS)
                     "                            create |\n"
                     "                            remove <name>\n"
# endif
                     "\n");
    }
#endif

    if (u64Cmd & USAGE_DHCPSERVER)
    {
        RTStrmPrintf(pStrm,
                     "VBoxManage dhcpserver       add|modify --netname <network_name> |\n"
#if defined(VBOX_WITH_NETFLT)
                     "                                       --ifname <hostonly_if_name>\n"
#endif
                     "                                [--ip <ip_address>\n"
                     "                                 --netmask <network_mask>\n"
                     "                                 --lowerip <lower_ip>\n"
                     "                                 --upperip <upper_ip>]\n"
                     "                                [--enable | --disable]\n\n"
                     "VBoxManage dhcpserver       remove --netname <network_name> |\n"
#if defined(VBOX_WITH_NETFLT)
                     "                                   --ifname <hostonly_if_name>\n"
#endif
                     "\n");
    }
}

/**
 * Print a usage synopsis and the syntax error message.
 */
int errorSyntax(USAGECATEGORY u64Cmd, const char *pszFormat, ...)
{
    va_list args;
    showLogo(g_pStdErr); // show logo even if suppressed
#ifndef VBOX_ONLY_DOCS
    if (g_fInternalMode)
        printUsageInternal(u64Cmd, g_pStdErr);
    else
        printUsage(u64Cmd, g_pStdErr);
#endif /* !VBOX_ONLY_DOCS */
    va_start(args, pszFormat);
    RTStrmPrintf(g_pStdErr, "\nSyntax error: %N\n", pszFormat, &args);
    va_end(args);
    return 1;
}

/**
 * errorSyntax for RTGetOpt users.
 *
 * @returns 1.
 *
 * @param   fUsageCategory  The usage category of the command.
 * @param   rc              The RTGetOpt return code.
 * @param   pValueUnion     The value union.
 */
int errorGetOpt(USAGECATEGORY fUsageCategory, int rc, union RTGETOPTUNION const *pValueUnion)
{
    showLogo(g_pStdErr); // show logo even if suppressed
#ifndef VBOX_ONLY_DOCS
    if (g_fInternalMode)
        printUsageInternal(fUsageCategory, g_pStdErr);
    else
        printUsage(fUsageCategory, g_pStdErr);
#endif /* !VBOX_ONLY_DOCS */

    if (rc == VINF_GETOPT_NOT_OPTION)
        return RTMsgError("Invalid parameter '%s'", pValueUnion->psz);
    if (rc > 0)
    {
        if (RT_C_IS_PRINT(rc))
            return RTMsgError("Invalid option -%c", rc);
        return RTMsgError("Invalid option case %i", rc);
    }
    if (rc == VERR_GETOPT_UNKNOWN_OPTION)
        return RTMsgError("Unknown option: %s", pValueUnion->psz);
    if (pValueUnion->pDef)
        return RTMsgError("%s: %Rrs", pValueUnion->pDef->pszLong, rc);
    return RTMsgError("%Rrs", rc);
}

/**
 * Print an error message without the syntax stuff.
 */
int errorArgument(const char *pszFormat, ...)
{
    va_list args;
    va_start(args, pszFormat);
    RTMsgError(pszFormat, args);
    va_end(args);
    return 1;
}
