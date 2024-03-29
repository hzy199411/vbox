/* $Id$ */
/** @file
 * VBoxManage - help and other message output.
 */

/*
 * Copyright (C) 2006-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <VBox/version.h>

#include <iprt/buildconfig.h>
#include <iprt/ctype.h>
#include <iprt/assert.h>
#include <iprt/env.h>
#include <iprt/err.h>
#include <iprt/getopt.h>
#include <iprt/stream.h>
#include <iprt/message.h>

#include "VBoxManage.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** If the usage is the given number of length long or longer, the error is
 * repeated so the user can actually see it. */
#define ERROR_REPEAT_AFTER_USAGE_LENGTH 16


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
#ifndef VBOX_ONLY_DOCS
static enum HELP_CMD_VBOXMANAGE    g_enmCurCommand = HELP_CMD_VBOXMANAGE_INVALID;
/** The scope mask for the current subcommand. */
static uint64_t                    g_fCurSubcommandScope = RTMSGREFENTRYSTR_SCOPE_GLOBAL;

/**
 * Sets the current command.
 *
 * This affects future calls to error and help functions.
 *
 * @param   enmCommand          The command.
 */
void setCurrentCommand(enum HELP_CMD_VBOXMANAGE enmCommand)
{
    Assert(g_enmCurCommand == HELP_CMD_VBOXMANAGE_INVALID);
    g_enmCurCommand       = enmCommand;
    g_fCurSubcommandScope = RTMSGREFENTRYSTR_SCOPE_GLOBAL;
}


/**
 * Sets the current subcommand.
 *
 * This affects future calls to error and help functions.
 *
 * @param   fSubcommandScope    The subcommand scope.
 */
void setCurrentSubcommand(uint64_t fSubcommandScope)
{
    g_fCurSubcommandScope = fSubcommandScope;
}




/**
 * Prints brief help for a command or subcommand.
 *
 * @returns Number of lines written.
 * @param   enmCommand          The command.
 * @param   fSubcommandScope    The subcommand scope, REFENTRYSTR_SCOPE_GLOBAL
 *                              for all.
 * @param   pStrm               The output stream.
 */
static uint32_t printBriefCommandOrSubcommandHelp(enum HELP_CMD_VBOXMANAGE enmCommand, uint64_t fSubcommandScope, PRTSTREAM pStrm)
{
    uint32_t cLinesWritten = 0;
    uint32_t cPendingBlankLines = 0;
    uint32_t cFound = 0;
    for (uint32_t i = 0; i < g_cHelpEntries; i++)
    {
        PCRTMSGREFENTRY pHelp = g_apHelpEntries[i];
        if (pHelp->idInternal == (int64_t)enmCommand)
        {
            cFound++;
            if (cFound == 1)
            {
                if (fSubcommandScope == RTMSGREFENTRYSTR_SCOPE_GLOBAL)
                    RTStrmPrintf(pStrm, "Usage - %c%s:\n", RT_C_TO_UPPER(pHelp->pszBrief[0]), pHelp->pszBrief + 1);
                else
                    RTStrmPrintf(pStrm, "Usage:\n");
            }
            RTMsgRefEntryPrintStringTable(pStrm, &pHelp->Synopsis, fSubcommandScope, &cPendingBlankLines, &cLinesWritten);
            if (!cPendingBlankLines)
                cPendingBlankLines = 1;
        }
    }
    Assert(cFound > 0);
    return cLinesWritten;
}


/**
 * Prints the brief usage information for the current (sub)command.
 *
 * @param   pStrm               The output stream.
 */
void printUsage(PRTSTREAM pStrm)
{
    printBriefCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, pStrm);
}


/**
 * Prints full help for a command or subcommand.
 *
 * @param   enmCommand          The command.
 * @param   fSubcommandScope    The subcommand scope, REFENTRYSTR_SCOPE_GLOBAL
 *                              for all.
 * @param   pStrm               The output stream.
 */
static void printFullCommandOrSubcommandHelp(enum HELP_CMD_VBOXMANAGE enmCommand, uint64_t fSubcommandScope, PRTSTREAM pStrm)
{
    uint32_t cPendingBlankLines = 0;
    uint32_t cFound = 0;
    for (uint32_t i = 0; i < g_cHelpEntries; i++)
    {
        PCRTMSGREFENTRY pHelp = g_apHelpEntries[i];
        if (   pHelp->idInternal == (int64_t)enmCommand
            || enmCommand == HELP_CMD_VBOXMANAGE_INVALID)
        {
            cFound++;
            RTMsgRefEntryPrintStringTable(pStrm, &pHelp->Help, fSubcommandScope, &cPendingBlankLines, NULL /*pcLinesWritten*/);
            if (cPendingBlankLines < 2)
                cPendingBlankLines = 2;
        }
    }
    Assert(cFound > 0);
}


/**
 * Prints the full help for the current (sub)command.
 *
 * @param   pStrm               The output stream.
 */
void printHelp(PRTSTREAM pStrm)
{
    printFullCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, pStrm);
}


/**
 * Display no subcommand error message and current command usage.
 *
 * @returns RTEXITCODE_SYNTAX.
 */
RTEXITCODE errorNoSubcommand(void)
{
    Assert(g_enmCurCommand != HELP_CMD_VBOXMANAGE_INVALID);
    Assert(g_fCurSubcommandScope == RTMSGREFENTRYSTR_SCOPE_GLOBAL);

    return errorSyntax("No subcommand specified");
}


/**
 * Display unknown subcommand error message and current command usage.
 *
 * May show full command help instead if the subcommand is a common help option.
 *
 * @returns RTEXITCODE_SYNTAX, or RTEXITCODE_SUCCESS if common help option.
 * @param   pszSubcommand       The name of the alleged subcommand.
 */
RTEXITCODE errorUnknownSubcommand(const char *pszSubcommand)
{
    Assert(g_enmCurCommand != HELP_CMD_VBOXMANAGE_INVALID);
    Assert(g_fCurSubcommandScope == RTMSGREFENTRYSTR_SCOPE_GLOBAL);

    /* check if help was requested. */
    if (   strcmp(pszSubcommand, "--help") == 0
        || strcmp(pszSubcommand, "-h") == 0
        || strcmp(pszSubcommand, "-?") == 0)
    {
        printFullCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, g_pStdOut);
        return RTEXITCODE_SUCCESS;
    }

    return errorSyntax("Unknown subcommand: %s", pszSubcommand);
}


/**
 * Display too many parameters error message and current command usage.
 *
 * May show full command help instead if the subcommand is a common help option.
 *
 * @returns RTEXITCODE_SYNTAX, or RTEXITCODE_SUCCESS if common help option.
 * @param   papszArgs           The first unwanted parameter.  Terminated by
 *                              NULL entry.
 */
RTEXITCODE errorTooManyParameters(char **papszArgs)
{
    Assert(g_enmCurCommand != HELP_CMD_VBOXMANAGE_INVALID);
    Assert(g_fCurSubcommandScope != RTMSGREFENTRYSTR_SCOPE_GLOBAL);

    /* check if help was requested. */
    if (papszArgs)
    {
        for (uint32_t i = 0; papszArgs[i]; i++)
            if (   strcmp(papszArgs[i], "--help") == 0
                || strcmp(papszArgs[i], "-h") == 0
                || strcmp(papszArgs[i], "-?") == 0)
            {
                printFullCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, g_pStdOut);
                return RTEXITCODE_SUCCESS;
            }
            else if (!strcmp(papszArgs[i], "--"))
                break;
    }

    return errorSyntax("Too many parameters");
}


/**
 * Display current (sub)command usage and the custom error message.
 *
 * @returns RTEXITCODE_SYNTAX.
 * @param   pszFormat           Custom error message format string.
 * @param   ...                 Format arguments.
 */
RTEXITCODE errorSyntax(const char *pszFormat, ...)
{
    Assert(g_enmCurCommand != HELP_CMD_VBOXMANAGE_INVALID);

    showLogo(g_pStdErr);

    va_list va;
    va_start(va, pszFormat);
    RTMsgErrorV(pszFormat, va);
    va_end(va);

    RTStrmPutCh(g_pStdErr, '\n');
    if (   printBriefCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, g_pStdErr)
        >= ERROR_REPEAT_AFTER_USAGE_LENGTH)
    {
        /* Usage was very long, repeat the error message. */
        RTStrmPutCh(g_pStdErr, '\n');
        va_start(va, pszFormat);
        RTMsgErrorV(pszFormat, va);
        va_end(va);
    }
    return RTEXITCODE_SYNTAX;
}


/**
 * Worker for errorGetOpt.
 *
 * @param   rcGetOpt            The RTGetOpt return value.
 * @param   pValueUnion         The value union returned by RTGetOpt.
 */
static void errorGetOptWorker(int rcGetOpt, union RTGETOPTUNION const *pValueUnion)
{
    if (rcGetOpt == VINF_GETOPT_NOT_OPTION)
        RTMsgError("Invalid parameter '%s'", pValueUnion->psz);
    else if (rcGetOpt > 0)
    {
        if (RT_C_IS_PRINT(rcGetOpt))
            RTMsgError("Invalid option -%c", rcGetOpt);
        else
            RTMsgError("Invalid option case %i", rcGetOpt);
    }
    else if (rcGetOpt == VERR_GETOPT_UNKNOWN_OPTION)
        RTMsgError("Unknown option: %s", pValueUnion->psz);
    else if (rcGetOpt == VERR_GETOPT_INVALID_ARGUMENT_FORMAT)
        RTMsgError("Invalid argument format: %s", pValueUnion->psz);
    else if (pValueUnion->pDef)
        RTMsgError("%s: %Rrs", pValueUnion->pDef->pszLong, rcGetOpt);
    else
        RTMsgError("%Rrs", rcGetOpt);
}


/**
 * For use to deal with RTGetOptFetchValue failures.
 *
 * @retval  RTEXITCODE_SYNTAX
 * @param   iValueNo            The value number being fetched, counting the
 *                              RTGetOpt value as zero and the first
 *                              RTGetOptFetchValue call as one.
 * @param   pszOption           The option being parsed.
 * @param   rcGetOptFetchValue  The status returned by RTGetOptFetchValue.
 * @param   pValueUnion         The value union returned by the fetch.
 */
RTEXITCODE errorFetchValue(int iValueNo, const char *pszOption, int rcGetOptFetchValue, union RTGETOPTUNION const *pValueUnion)
{
    Assert(g_enmCurCommand != HELP_CMD_VBOXMANAGE_INVALID);
    showLogo(g_pStdErr);
    if (rcGetOptFetchValue == VERR_GETOPT_REQUIRED_ARGUMENT_MISSING)
        RTMsgError("Missing the %u%s value for option %s",
                   iValueNo, iValueNo == 1 ? "st" : iValueNo == 2 ? "nd" : iValueNo == 3 ? "rd" : "th",  pszOption);
    else
        errorGetOptWorker(rcGetOptFetchValue, pValueUnion);
    return RTEXITCODE_SYNTAX;

}


/**
 * Handled an RTGetOpt error or common option.
 *
 * This implements the 'V' and 'h' cases.  It reports appropriate syntax errors
 * for other @a rcGetOpt values.
 *
 * @retval  RTEXITCODE_SUCCESS if help or version request.
 * @retval  RTEXITCODE_SYNTAX if not help or version request.
 * @param   rcGetOpt            The RTGetOpt return value.
 * @param   pValueUnion         The value union returned by RTGetOpt.
 */
RTEXITCODE errorGetOpt(int rcGetOpt, union RTGETOPTUNION const *pValueUnion)
{
    Assert(g_enmCurCommand != HELP_CMD_VBOXMANAGE_INVALID);

    /*
     * Check if it is an unhandled standard option.
     */
    if (rcGetOpt == 'V')
    {
        RTPrintf("%sr%d\n", VBOX_VERSION_STRING, RTBldCfgRevision());
        return RTEXITCODE_SUCCESS;
    }

    if (rcGetOpt == 'h')
    {
        printFullCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, g_pStdOut);
        return RTEXITCODE_SUCCESS;
    }

    /*
     * We failed.
     */
    showLogo(g_pStdErr);
    errorGetOptWorker(rcGetOpt, pValueUnion);
    if (   printBriefCommandOrSubcommandHelp(g_enmCurCommand, g_fCurSubcommandScope, g_pStdErr)
        >= ERROR_REPEAT_AFTER_USAGE_LENGTH)
    {
        /* Usage was very long, repeat the error message. */
        RTStrmPutCh(g_pStdErr, '\n');
        errorGetOptWorker(rcGetOpt, pValueUnion);
    }
    return RTEXITCODE_SYNTAX;
}

#endif /* !VBOX_ONLY_DOCS */



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




void printUsage(USAGECATEGORY enmCommand, uint64_t fSubcommandScope, PRTSTREAM pStrm)
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
#ifdef VBOX_WITH_VBOXSDL
    bool fVBoxSDL = true;
#else
    bool fVBoxSDL = false;
#endif

    Assert(enmCommand != USAGE_INVALID);
    Assert(enmCommand != USAGE_S_NEWCMD);

    if (enmCommand == USAGE_S_DUMPOPTS)
    {
        fDumpOpts = true;
        fLinux = true;
        fWin = true;
        fSolaris = true;
        fFreeBSD = true;
        fDarwin = true;
        fVBoxSDL = true;
        enmCommand = USAGE_S_ALL;
    }

    RTStrmPrintf(pStrm,
                 "Usage:\n"
                 "\n");

    if (enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                     "  VBoxManage [<general option>] <command>\n"
                     "\n"
                     "\n"
                     "General Options:\n"
                     "\n"
                     "  [-V|--version]            print version number and exit\n"
                     "  [--dump-build-type]       print build type and exit\n"
                     "  [-q|--nologo]             suppress the logo\n"
                     "  [--settingspw <pw>]       provide the settings password\n"
                     "  [--settingspwfile <file>] provide a file containing the settings password\n"
                     "  [@<response-file>]        load arguments from the given response file (bourne style)\n"
                     "\n"
                     "\n"
                     "Commands:\n"
                     "\n");

    const char *pcszSep1 = " ";
    const char *pcszSep2 = "         ";
    if (enmCommand != USAGE_S_ALL)
    {
        pcszSep1 = "VBoxManage";
        pcszSep2 = "";
    }

#define SEP pcszSep1, pcszSep2

    if (enmCommand == USAGE_STARTVM || enmCommand == USAGE_S_ALL)
    {
        RTStrmPrintf(pStrm,
                           "%s startvm %s         <uuid|vmname>...\n"
                     "                            [--type gui", SEP);
        if (fVBoxSDL)
            RTStrmPrintf(pStrm, "|sdl");
        RTStrmPrintf(pStrm, "|headless|separate]\n");
        RTStrmPrintf(pStrm,
                     "                            [-E|--putenv <NAME>[=<VALUE>]]\n"
                     "\n");
    }

    if (enmCommand == USAGE_CONTROLVM || enmCommand == USAGE_S_ALL)
    {
        RTStrmPrintf(pStrm,
                           "%s controlvm %s       <uuid|vmname>\n"
                     "                            pause|resume|reset|poweroff|savestate|\n"
#ifdef VBOX_WITH_GUEST_CONTROL
                     "                            reboot|shutdown [--force]|\n"
#endif
                     "                            acpipowerbutton|acpisleepbutton|\n"
                     "                            keyboardputscancode <hex> [<hex> ...]|\n"
                     "                            keyboardputstring <string1> [<string2> ...]|\n"
                     "                            keyboardputfile <filename>|\n"
                     "                            setlinkstate<1-N> on|off |\n"
#if defined(VBOX_WITH_NETFLT)
                     "                            nic<1-N> null|nat|bridged|intnet|hostonly|generic|\n"
                     "                                     natnetwork [<devicename>] |\n"
#else /* !VBOX_WITH_NETFLT */
                     "                            nic<1-N> null|nat|bridged|intnet|generic|natnetwork\n"
                     "                                     [<devicename>] |\n"
#endif /* !VBOX_WITH_NETFLT */
                     "                            nictrace<1-N> on|off |\n"
                     "                            nictracefile<1-N> <filename> |\n"
                     "                            nicproperty<1-N> name=[value] |\n"
                     "                            nicpromisc<1-N> deny|allow-vms|allow-all |\n"
                     "                            natpf<1-N> [<rulename>],tcp|udp,[<hostip>],\n"
                     "                                        <hostport>,[<guestip>],<guestport> |\n"
                     "                            natpf<1-N> delete <rulename> |\n"
                     "                            guestmemoryballoon <balloonsize in MB> |\n"
                     "                            usbattach <uuid>|<address>\n"
                     "                                      [--capturefile <filename>] |\n"
                     "                            usbdetach <uuid>|<address> |\n"
                     "                            audioin on|off |\n"
                     "                            audioout on|off |\n"
#ifdef VBOX_WITH_SHARED_CLIPBOARD
                     "                            clipboard mode disabled|hosttoguest|guesttohost|\n"
                     "                                           bidirectional |\n"
# ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
                     "                            clipboard filetransfers enabled|disabled |\n"
# endif
#endif
                     "                            draganddrop disabled|hosttoguest|guesttohost|\n"
                     "                                        bidirectional |\n"
                     "                            vrde on|off |\n"
                     "                            vrdeport <port> |\n"
                     "                            vrdeproperty <name=[value]> |\n"
                     "                            vrdevideochannelquality <percent> |\n"
                     "                            setvideomodehint <xres> <yres> <bpp>\n"
                     "                                            [[<display>] [<enabled:yes|no> |\n"
                     "                                              [<xorigin> <yorigin>]]] |\n"
                     "                            setscreenlayout <display> on|primary <xorigin> <yorigin> <xres> <yres> <bpp> | off\n"
                     "                            screenshotpng <file> [display] |\n"
#ifdef VBOX_WITH_RECORDING
                     "                            recording on|off |\n"
                     "                            recording screens all|none|<screen>,[<screen>...] |\n"
                     "                            recording filename <file> |\n"
                     "                            recording videores <width>x<height> |\n"
                     "                            recording videorate <rate> |\n"
                     "                            recording videofps <fps> |\n"
                     "                            recording maxtime <s> |\n"
                     "                            recording maxfilesize <MB> |\n"
#endif /* VBOX_WITH_RECORDING */
                     "                            setcredentials <username>\n"
                     "                                           --passwordfile <file> | <password>\n"
                     "                                           <domain>\n"
                     "                                           [--allowlocallogon <yes|no>] |\n"
                     "                            teleport --host <name> --port <port>\n"
                     "                                     [--maxdowntime <msec>]\n"
                     "                                     [--passwordfile <file> |\n"
                     "                                      --password <password>] |\n"
                     "                            plugcpu <id> |\n"
                     "                            unplugcpu <id> |\n"
                     "                            cpuexecutioncap <1-100>\n"
                     "                            webcam <attach [path [settings]]> | <detach [path]> | <list>\n"
                     "                            addencpassword <id>\n"
                     "                                           <password file>|-\n"
                     "                                           [--removeonsuspend <yes|no>]\n"
                     "                            removeencpassword <id>\n"
                     "                            removeallencpasswords\n"
                     "                            changeuartmode<1-N> disconnected|\n"
                     "                                                server <pipe>|\n"
                     "                                                client <pipe>|\n"
                     "                                                tcpserver <port>|\n"
                     "                                                tcpclient <hostname:port>|\n"
                     "                                                file <file>|\n"
                     "                                                <devicename>\n"
                     "                            vm-process-priority default|flat|low|normal|high\n"
                     "                            autostart-enabled on|off\n"
                     "                            autostart-delay <seconds>\n"
                     "\n", SEP);
    }

    if (enmCommand == USAGE_DISCARDSTATE || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s discardstate %s    <uuid|vmname>\n"
                     "\n", SEP);

    if (enmCommand == USAGE_ADOPTSTATE || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s adoptstate %s      <uuid|vmname> <state_file>\n"
                     "\n", SEP);

    if (enmCommand == USAGE_CLOSEMEDIUM || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s closemedium %s     [disk|dvd|floppy] <uuid|filename>\n"
                     "                            [--delete]\n"
                     "\n", SEP);

    if (enmCommand == USAGE_STORAGEATTACH || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s storageattach %s   <uuid|vmname>\n"
                     "                            --storagectl <name>\n"
                     "                            [--port <number>]\n"
                     "                            [--device <number>]\n"
                     "                            [--type dvddrive|hdd|fdd]\n"
                     "                            [--medium none|emptydrive|additions|\n"
                     "                                      <uuid|filename>|host:<drive>|iscsi]\n"
                     "                            [--mtype normal|writethrough|immutable|shareable|\n"
                     "                                     readonly|multiattach]\n"
                     "                            [--comment <text>]\n"
                     "                            [--setuuid <uuid>]\n"
                     "                            [--setparentuuid <uuid>]\n"
                     "                            [--passthrough on|off]\n"
                     "                            [--tempeject on|off]\n"
                     "                            [--nonrotational on|off]\n"
                     "                            [--discard on|off]\n"
                     "                            [--hotpluggable on|off]\n"
                     "                            [--bandwidthgroup <name>]\n"
                     "                            [--forceunmount]\n"
                     "                            [--server <name>|<ip>]\n"
                     "                            [--target <target>]\n"
                     "                            [--tport <port>]\n"
                     "                            [--lun <lun>]\n"
                     "                            [--encodedlun <lun>]\n"
                     "                            [--username <username>]\n"
                     "                            [--password <password>]\n"
                     "                            [--passwordfile <file>]\n"
                     "                            [--initiator <initiator>]\n"
                     "                            [--intnet]\n"
                     "\n", SEP);

    if (enmCommand == USAGE_STORAGECONTROLLER || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s storagectl %s      <uuid|vmname>\n"
                     "                            --name <name>\n"
                     "                            [--add ide|sata|scsi|floppy|sas|usb|pcie|virtio]\n"
                     "                            [--controller LSILogic|LSILogicSAS|BusLogic|\n"
                     "                                          IntelAHCI|PIIX3|PIIX4|ICH6|I82078|\n"
                     "                            [             USB|NVMe|VirtIO]\n"
                     "                            [--portcount <1-n>]\n"
                     "                            [--hostiocache on|off]\n"
                     "                            [--bootable on|off]\n"
                     "                            [--rename <name>]\n"
                     "                            [--remove]\n"
                     "\n", SEP);

    if (enmCommand == USAGE_BANDWIDTHCONTROL || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s bandwidthctl %s    <uuid|vmname>\n"
                     "                            add <name> --type disk|network\n"
                     "                                --limit <megabytes per second>[k|m|g|K|M|G] |\n"
                     "                            set <name>\n"
                     "                                --limit <megabytes per second>[k|m|g|K|M|G] |\n"
                     "                            remove <name> |\n"
                     "                            list [--machinereadable]\n"
                     "                            (limit units: k=kilobit, m=megabit, g=gigabit,\n"
                     "                                          K=kilobyte, M=megabyte, G=gigabyte)\n"
                     "\n", SEP);

    if (enmCommand == USAGE_SHOWMEDIUMINFO || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s showmediuminfo %s  [disk|dvd|floppy] <uuid|filename>\n"
                     "\n", SEP);

    if (enmCommand == USAGE_CREATEMEDIUM || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s createmedium %s    [disk|dvd|floppy] --filename <filename>\n"
                     "                            [--size <megabytes>|--sizebyte <bytes>]\n"
                     "                            [--diffparent <uuid>|<filename>]\n"
                     "                            [--format VDI|VMDK|VHD] (default: VDI)]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX,\n"
                     "                                       Formatted,RawDisk]\n"
                     "                            [[--property <name>=<value>] --property <name>=<value>\n"
                     "                              --property-file <name>=</path/to/file/with/value>]...\n"
                     "\n", SEP);

    if (enmCommand == USAGE_MODIFYMEDIUM || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s modifymedium %s    [disk|dvd|floppy] <uuid|filename>\n"
                     "                            [--type normal|writethrough|immutable|shareable|\n"
                     "                                    readonly|multiattach]\n"
                     "                            [--autoreset on|off]\n"
                     "                            [--property <name=[value]>]\n"
                     "                            [--compact]\n"
                     "                            [--resize <megabytes>|--resizebyte <bytes>]\n"
                     "                            [--move <path>]\n"
                     "                            [--setlocation <path>]\n"
                     "                            [--description <description string>]"
                     "\n", SEP);

    if (enmCommand == USAGE_CLONEMEDIUM || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s clonemedium %s     [disk|dvd|floppy] <uuid|inputfile> <uuid|outputfile>\n"
                     "                            [--format VDI|VMDK|VHD|RAW|<other>]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
                     "                            [--existing]\n"
                     "\n", SEP);

    if (enmCommand == USAGE_MEDIUMPROPERTY || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s mediumproperty %s  [disk|dvd|floppy] set <uuid|filename>\n"
                     "                            <property> <value>\n"
                     "\n"
                     "                            [disk|dvd|floppy] get <uuid|filename>\n"
                     "                            <property>\n"
                     "\n"
                     "                            [disk|dvd|floppy] delete <uuid|filename>\n"
                     "                            <property>\n"
                     "\n", SEP);

    if (enmCommand == USAGE_ENCRYPTMEDIUM || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s encryptmedium %s   <uuid|filename>\n"
                     "                            [--newpassword <file>|-]\n"
                     "                            [--oldpassword <file>|-]\n"
                     "                            [--cipher <cipher identifier>]\n"
                     "                            [--newpasswordid <password identifier>]\n"
                     "\n", SEP);

    if (enmCommand == USAGE_MEDIUMENCCHKPWD || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s checkmediumpwd %s  <uuid|filename>\n"
                     "                            <pwd file>|-\n"
                     "\n", SEP);

    if (enmCommand == USAGE_CONVERTFROMRAW || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s convertfromraw %s  <filename> <outputfile>\n"
                     "                            [--format VDI|VMDK|VHD]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
                     "                            [--uuid <uuid>]\n"
                           "%s convertfromraw %s  stdin <outputfile> <bytes>\n"
                     "                            [--format VDI|VMDK|VHD]\n"
                     "                            [--variant Standard,Fixed,Split2G,Stream,ESX]\n"
                     "                            [--uuid <uuid>]\n"
                     "\n", SEP, SEP);

    if (enmCommand == USAGE_GETEXTRADATA || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s getextradata %s    global|<uuid|vmname>\n"
                     "                            <key>|[enumerate]\n"
                     "\n", SEP);

    if (enmCommand == USAGE_SETEXTRADATA || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s setextradata %s    global|<uuid|vmname>\n"
                     "                            <key>\n"
                     "                            [<value>] (no value deletes key)\n"
                     "\n", SEP);

    if (enmCommand == USAGE_SETPROPERTY || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s setproperty %s     machinefolder default|<folder> |\n"
                     "                            hwvirtexclusive on|off |\n"
                     "                            vrdeauthlibrary default|<library> |\n"
                     "                            websrvauthlibrary default|null|<library> |\n"
                     "                            vrdeextpack null|<library> |\n"
                     "                            autostartdbpath null|<folder> |\n"
                     "                            loghistorycount <value>\n"
                     "                            defaultfrontend default|<name>\n"
                     "                            logginglevel <log setting>\n"
                     "                            proxymode system|noproxy|manual\n"
                     "                            proxyurl <url>\n"
#ifdef VBOX_WITH_MAIN_NLS
                     "                            language <language id>\n"
#endif
                     "\n", SEP);

    if (enmCommand == USAGE_USBFILTER || enmCommand == USAGE_S_ALL)
    {
        if (fSubcommandScope & HELP_SCOPE_USBFILTER_ADD)
            RTStrmPrintf(pStrm,
                               "%s usbfilter %s       add <index,0-N>\n"
                         "                            --target <uuid|vmname>|global\n"
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
                         "\n", SEP);

        if (fSubcommandScope & HELP_SCOPE_USBFILTER_MODIFY)
            RTStrmPrintf(pStrm,
                               "%s usbfilter %s       modify <index,0-N>\n"
                         "                            --target <uuid|vmname>|global\n"
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
                         "\n", SEP);

        if (fSubcommandScope & HELP_SCOPE_USBFILTER_REMOVE)
            RTStrmPrintf(pStrm,
                               "%s usbfilter %s       remove <index,0-N>\n"
                         "                            --target <uuid|vmname>|global\n"
                         "\n", SEP);
    }

#ifdef VBOX_WITH_GUEST_PROPS
    if (enmCommand == USAGE_GUESTPROPERTY || enmCommand == USAGE_S_ALL)
        usageGuestProperty(pStrm, SEP);
#endif /* VBOX_WITH_GUEST_PROPS defined */

#ifdef VBOX_WITH_GUEST_CONTROL
    if (enmCommand == USAGE_GUESTCONTROL || enmCommand == USAGE_S_ALL)
        usageGuestControl(pStrm, SEP, fSubcommandScope);
#endif /* VBOX_WITH_GUEST_CONTROL defined */

    if (enmCommand == USAGE_METRICS || enmCommand == USAGE_S_ALL)
        RTStrmPrintf(pStrm,
                           "%s metrics %s         list [*|host|<vmname> [<metric_list>]]\n"
                     "                                                 (comma-separated)\n\n"
                           "%s metrics %s         setup\n"
                     "                            [--period <seconds>] (default: 1)\n"
                     "                            [--samples <count>] (default: 1)\n"
                     "                            [--list]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n\n"
                           "%s metrics %s         query [*|host|<vmname> [<metric_list>]]\n\n"
                           "%s metrics %s         enable\n"
                     "                            [--list]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n\n"
                           "%s metrics %s         disable\n"
                     "                            [--list]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n\n"
                           "%s metrics %s         collect\n"
                     "                            [--period <seconds>] (default: 1)\n"
                     "                            [--samples <count>] (default: 1)\n"
                     "                            [--list]\n"
                     "                            [--detach]\n"
                     "                            [*|host|<vmname> [<metric_list>]]\n"
                     "\n", SEP, SEP, SEP, SEP, SEP, SEP);

#if defined(VBOX_WITH_NAT_SERVICE)
    if (enmCommand == USAGE_NATNETWORK || enmCommand == USAGE_S_ALL)
    {
        RTStrmPrintf(pStrm,
                           "%s natnetwork %s      add --netname <name>\n"
                     "                            --network <network>\n"
                     "                            [--enable|--disable]\n"
                     "                            [--dhcp on|off]\n"
                     "                            [--port-forward-4 <rule>]\n"
                     "                            [--loopback-4 <rule>]\n"
                     "                            [--ipv6 on|off]\n"
                     "                            [--port-forward-6 <rule>]\n"
                     "                            [--loopback-6 <rule>]\n\n"
                           "%s natnetwork %s      remove --netname <name>\n\n"
                           "%s natnetwork %s      modify --netname <name>\n"
                     "                            [--network <network>]\n"
                     "                            [--enable|--disable]\n"
                     "                            [--dhcp on|off]\n"
                     "                            [--port-forward-4 <rule>]\n"
                     "                            [--loopback-4 <rule>]\n"
                     "                            [--ipv6 on|off]\n"
                     "                            [--port-forward-6 <rule>]\n"
                     "                            [--loopback-6 <rule>]\n\n"
                           "%s natnetwork %s      start --netname <name>\n\n"
                           "%s natnetwork %s      stop --netname <name>\n\n"
                           "%s natnetwork %s      list [<pattern>]\n"
                     "\n", SEP, SEP, SEP, SEP, SEP, SEP);


    }
#endif

#if defined(VBOX_WITH_NETFLT)
    if (enmCommand == USAGE_HOSTONLYIFS || enmCommand == USAGE_S_ALL)
    {
        RTStrmPrintf(pStrm,
                           "%s hostonlyif %s      ipconfig <name>\n"
                     "                            [--dhcp |\n"
                     "                            --ip<ipv4> [--netmask<ipv4> (def: 255.255.255.0)] |\n"
                     "                            --ipv6<ipv6> [--netmasklengthv6<length> (def: 64)]]\n"
# if !defined(RT_OS_SOLARIS) || defined(VBOX_ONLY_DOCS)
                     "                            create |\n"
                     "                            remove <name>\n"
# endif
                     "\n", SEP);
    }
#endif

    if (enmCommand == USAGE_USBDEVSOURCE || enmCommand == USAGE_S_ALL)
    {
        RTStrmPrintf(pStrm,
                           "%s usbdevsource %s    add <source name>\n"
                     "                            --backend <backend>\n"
                     "                            --address <address>\n"
                           "%s usbdevsource %s    remove <source name>\n"
                     "\n", SEP, SEP);
    }

#ifndef VBOX_ONLY_DOCS /* Converted to man page, not needed. */
    if (enmCommand == USAGE_S_ALL)
    {
        uint32_t cPendingBlankLines = 0;
        for (uint32_t i = 0; i < g_cHelpEntries; i++)
        {
            PCRTMSGREFENTRY pHelp = g_apHelpEntries[i];
            while (cPendingBlankLines-- > 0)
                RTStrmPutCh(pStrm, '\n');
            RTStrmPrintf(pStrm, " %c%s:\n", RT_C_TO_UPPER(pHelp->pszBrief[0]), pHelp->pszBrief + 1);
            cPendingBlankLines = 0;
            RTMsgRefEntryPrintStringTable(pStrm, &pHelp->Synopsis, RTMSGREFENTRYSTR_SCOPE_GLOBAL,
                                          &cPendingBlankLines, NULL /*pcLinesWritten*/);
            cPendingBlankLines = RT_MAX(cPendingBlankLines, 1);
        }
    }
#endif
}

/**
 * Print a usage synopsis and the syntax error message.
 * @returns RTEXITCODE_SYNTAX.
 */
RTEXITCODE errorSyntax(USAGECATEGORY enmCommand, const char *pszFormat, ...)
{
    va_list args;
    showLogo(g_pStdErr); // show logo even if suppressed
#ifndef VBOX_ONLY_DOCS
    if (g_fInternalMode)
        printUsageInternal(enmCommand, g_pStdErr);
    else if (g_enmCurCommand == HELP_CMD_VBOXMANAGE_INVALID)
        printUsage(enmCommand, RTMSGREFENTRYSTR_SCOPE_GLOBAL, g_pStdErr);
    else
        printUsage(g_pStdErr);
#else
    RT_NOREF_PV(enmCommand);
#endif
    va_start(args, pszFormat);
    RTStrmPrintf(g_pStdErr, "\nSyntax error: %N\n", pszFormat, &args);
    va_end(args);
    return RTEXITCODE_SYNTAX;
}

/**
 * Print a usage synopsis and the syntax error message.
 * @returns RTEXITCODE_SYNTAX.
 */
RTEXITCODE errorSyntaxEx(USAGECATEGORY enmCommand, uint64_t fSubcommandScope, const char *pszFormat, ...)
{
    va_list args;
    showLogo(g_pStdErr); // show logo even if suppressed
#ifndef VBOX_ONLY_DOCS
    if (g_fInternalMode)
        printUsageInternal(enmCommand, g_pStdErr);
    else if (g_enmCurCommand == HELP_CMD_VBOXMANAGE_INVALID)
        printUsage(enmCommand, fSubcommandScope, g_pStdErr);
    else
        printUsage(g_pStdErr);
#else
    RT_NOREF2(enmCommand, fSubcommandScope);
#endif
    va_start(args, pszFormat);
    RTStrmPrintf(g_pStdErr, "\nSyntax error: %N\n", pszFormat, &args);
    va_end(args);
    return RTEXITCODE_SYNTAX;
}

/**
 * errorSyntax for RTGetOpt users.
 *
 * @returns RTEXITCODE_SYNTAX.
 *
 * @param   enmCommand          The command.
 * @param   fSubcommandScope    The subcommand scope, REFENTRYSTR_SCOPE_GLOBAL
 *                              for all.
 * @param   rc                  The RTGetOpt return code.
 * @param   pValueUnion         The value union.
 */
RTEXITCODE errorGetOptEx(USAGECATEGORY enmCommand, uint64_t fSubcommandScope, int rc, union RTGETOPTUNION const *pValueUnion)
{
    /*
     * Check if it is an unhandled standard option.
     */
#ifndef VBOX_ONLY_DOCS
    if (rc == 'V')
    {
        RTPrintf("%sr%d\n", VBOX_VERSION_STRING, RTBldCfgRevision());
        return RTEXITCODE_SUCCESS;
    }
#endif

    if (rc == 'h')
    {
        showLogo(g_pStdErr);
#ifndef VBOX_ONLY_DOCS
        if (g_fInternalMode)
            printUsageInternal(enmCommand, g_pStdOut);
        else if (g_enmCurCommand == HELP_CMD_VBOXMANAGE_INVALID)
            printUsage(enmCommand, fSubcommandScope, g_pStdOut);
        else
            printUsage(g_pStdErr);
#endif
        return RTEXITCODE_SUCCESS;
    }

    /*
     * General failure.
     */
    showLogo(g_pStdErr); // show logo even if suppressed
#ifndef VBOX_ONLY_DOCS
    if (g_fInternalMode)
        printUsageInternal(enmCommand, g_pStdErr);
    else if (g_enmCurCommand == HELP_CMD_VBOXMANAGE_INVALID)
        printUsage(enmCommand, fSubcommandScope, g_pStdErr);
    else
        printUsage(g_pStdErr);
#else
    RT_NOREF2(enmCommand, fSubcommandScope);
#endif

    if (rc == VINF_GETOPT_NOT_OPTION)
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid parameter '%s'", pValueUnion->psz);
    if (rc > 0)
    {
        if (RT_C_IS_PRINT(rc))
            return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid option -%c", rc);
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid option case %i", rc);
    }
    if (rc == VERR_GETOPT_UNKNOWN_OPTION)
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Unknown option: %s", pValueUnion->psz);
    if (rc == VERR_GETOPT_INVALID_ARGUMENT_FORMAT)
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid argument format: %s", pValueUnion->psz);
    if (pValueUnion->pDef)
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "%s: %Rrs", pValueUnion->pDef->pszLong, rc);
    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "%Rrs", rc);
}

/**
 * errorSyntax for RTGetOpt users.
 *
 * @returns RTEXITCODE_SYNTAX.
 *
 * @param   enmCommand      The command.
 * @param   rc              The RTGetOpt return code.
 * @param   pValueUnion     The value union.
 */
RTEXITCODE errorGetOpt(USAGECATEGORY enmCommand, int rc, union RTGETOPTUNION const *pValueUnion)
{
    return errorGetOptEx(enmCommand, RTMSGREFENTRYSTR_SCOPE_GLOBAL, rc, pValueUnion);
}

/**
 * Print an error message without the syntax stuff.
 *
 * @returns RTEXITCODE_SYNTAX.
 */
RTEXITCODE errorArgument(const char *pszFormat, ...)
{
    va_list args;
    va_start(args, pszFormat);
    RTMsgErrorV(pszFormat, args);
    va_end(args);
    return RTEXITCODE_SYNTAX;
}

