#!/usr/bin/env python
# -*- coding: utf-8 -*-
# pylint: disable=too-many-lines

"""
VirtualBox Validation Kit - Guest Control Tests.
"""

__copyright__ = \
"""
Copyright (C) 2010-2019 Oracle Corporation

This file is part of VirtualBox Open Source Edition (OSE), as
available from http://www.virtualbox.org. This file is free software;
you can redistribute it and/or modify it under the terms of the GNU
General Public License (GPL) as published by the Free Software
Foundation, in version 2 as it comes in the "COPYING" file of the
VirtualBox OSE distribution. VirtualBox OSE is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.

The contents of this file may alternatively be used under the terms
of the Common Development and Distribution License Version 1.0
(CDDL) only, as it comes in the "COPYING.CDDL" file of the
VirtualBox OSE distribution, in which case the provisions of the
CDDL are applicable instead of those of the GPL.

You may elect to license modified versions of this file under the
terms and conditions of either the GPL or the CDDL or both.
"""
__version__ = "$Revision$"

# Standard Python imports.
from array import array
import errno
import os
import random
import string
import struct
import sys
import threading
import time

# Only the main script needs to modify the path.
try:    __file__
except: __file__ = sys.argv[0];
g_ksValidationKitDir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))));
sys.path.append(g_ksValidationKitDir);

# Validation Kit imports.
from testdriver import reporter;
from testdriver import base;
from testdriver import vbox;
from testdriver import vboxcon;
from testdriver import vboxwrappers;
from common     import utils;

# Python 3 hacks:
if sys.version_info[0] >= 3:
    long = int      # pylint: disable=redefined-builtin,invalid-name
    xrange = range; # pylint: disable=redefined-builtin,invalid-name


class GuestStream(bytearray):
    """
    Class for handling a guest process input/output stream.
    """
    def appendStream(self, stream, convertTo = '<b'):
        """
        Appends and converts a byte sequence to this object;
        handy for displaying a guest stream.
        """
        self.extend(struct.pack(convertTo, stream));

class tdCtxTest(object):
    """
    Provides the actual test environment.
    Should be kept as generic as possible.
    """
    def __init__(self, oSession, oTxsSession, oTestVm): # pylint: disable=unused-argument
        ## The desired Main API result.
        self.fRc = False;
        ## IGuest reference.
        self.oGuest      = oSession.o.console.guest; ## @todo may throw shit
        self.oSesison    = oSession;
        self.oTxsSesison = oTxsSession;
        self.oTestVm     = oTestVm;

class tdCtxCreds(object):
    """
    Provides credentials to pass to the guest.
    """
    def __init__(self, sUser = None, sPassword = None, sDomain = None):
        self.oTestVm   = None;
        self.sUser     = sUser;
        self.sPassword = sPassword;
        self.sDomain   = sDomain;

    def applyDefaultsIfNotSet(self, oTestVm):
        """
        Applies credential defaults, based on the test VM (guest OS), if
        no credentials were set yet.
        """
        self.oTestVm = oTestVm;
        assert self.oTestVm is not None;

        if self.sUser is None:
            self.sUser = self.oTestVm.getTestUser();

        if self.sPassword is None:
            self.sPassword = self.oTestVm.getTestUserPassword(self.sUser);

        if self.sDomain is None:
            self.sDomain   = '';

class tdTestGuestCtrlBase(object):
    """
    Base class for all guest control tests.

    Note: This test ASSUMES that working Guest Additions
          were installed and running on the guest to be tested.
    """
    def __init__(self):
        self.oTest  = None;
        self.oCreds = None;
        self.timeoutMS = 30 * 1000; # 30s timeout
        ## IGuestSession reference or None.
        self.oGuestSession = None;

    def setEnvironment(self, oSession, oTxsSession, oTestVm):
        """
        Sets the test environment required for this test.
        """
        self.oTest = tdCtxTest(oSession, oTxsSession, oTestVm);
        self.oCreds.applyDefaultsIfNotSet(oTestVm);
        return self.oTest;

    def uploadLogData(self, oTstDrv, aData, sFileName, sDesc):
        """
        Uploads (binary) data to a log file for manual (later) inspection.
        """
        reporter.log('Creating + uploading log data file "%s"' % sFileName);
        sHstFileName = os.path.join(oTstDrv.sScratchPath, sFileName);
        try:
            oCurTestFile = open(sHstFileName, "wb");
            oCurTestFile.write(aData);
            oCurTestFile.close();
        except:
            return reporter.error('Unable to create temporary file for "%s"' % (sDesc,));
        return reporter.addLogFile(sHstFileName, 'misc/other', sDesc);

    def createSession(self, sName, fIsError = False):
        """
        Creates (opens) a guest session.
        Returns (True, IGuestSession) on success or (False, None) on failure.
        """
        if self.oGuestSession is None:
            if sName is None:
                sName = "<untitled>";

            reporter.log('Creating session "%s" ...' % (sName,));
            try:
                self.oGuestSession = self.oTest.oGuest.createSession(self.oCreds.sUser,
                                                                     self.oCreds.sPassword,
                                                                     self.oCreds.sDomain,
                                                                     sName);
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.maybeErrXcpt(fIsError, 'Creating a guest session "%s" failed; sUser="%s", pw="%s", sDomain="%s":'
                                      % (sName, self.oCreds.sUser, self.oCreds.sPassword, self.oCreds.sDomain));
                return (False, None);

            reporter.log('Waiting for session "%s" to start within %dms...' % (sName, self.timeoutMS));
            aeWaitFor = [ vboxcon.GuestSessionWaitForFlag_Start, ];
            try:
                waitResult = self.oGuestSession.waitForArray(aeWaitFor, self.timeoutMS);

                #
                # Be nice to Guest Additions < 4.3: They don't support session handling and
                # therefore return WaitFlagNotSupported.
                #
                if waitResult not in (vboxcon.GuestSessionWaitResult_Start, vboxcon.GuestSessionWaitResult_WaitFlagNotSupported):
                    # Just log, don't assume an error here (will be done in the main loop then).
                    reporter.maybeErr(fIsError, 'Session did not start successfully, returned wait result: %d' % (waitResult,));
                    return (False, None);
                reporter.log('Session "%s" successfully started' % (sName,));
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.maybeErrXcpt(fIsError, 'Waiting for guest session "%s" (usr=%s;pw=%s;dom=%s) to start failed:'
                                      % (sName, self.oCreds.sUser, self.oCreds.sPassword, self.oCreds.sDomain,));
                return (False, None);
        else:
            reporter.log('Warning: Session already set; this is probably not what you want');
        return (True, self.oGuestSession);

    def setSession(self, oGuestSession):
        """
        Sets the current guest session and closes
        an old one if necessary.
        """
        if self.oGuestSession is not None:
            self.closeSession();
        self.oGuestSession = oGuestSession;
        return self.oGuestSession;

    def closeSession(self, fIsError = False):
        """
        Closes the guest session.
        """
        if self.oGuestSession is not None:
            try:
                sName = self.oGuestSession.name;
            except:
                return reporter.errorXcpt();

            reporter.log('Closing session "%s" ...' % (sName,));
            try:
                self.oGuestSession.close();
                self.oGuestSession = None;
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.maybeErrXcpt(fIsError, 'Closing guest session "%s" failed:' % (sName,));
                return False;
        return True;

class tdTestCopyFrom(tdTestGuestCtrlBase):
    """
    Test for copying files from the guest to the host.
    """
    def __init__(self, sSrc = "", sDst = "", sUser = None, sPassword = None, fFlags = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sSrc = sSrc;
        self.sDst = sDst;
        self.fFlags = fFlags;

class tdTestCopyTo(tdTestGuestCtrlBase):
    """
    Test for copying files from the host to the guest.
    """
    def __init__(self, sSrc = "", sDst = "", sUser = None, sPassword = None, fFlags = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sSrc = sSrc;
        self.sDst = sDst;
        self.fFlags = fFlags;

class tdTestDirCreate(tdTestGuestCtrlBase):
    """
    Test for directoryCreate call.
    """
    def __init__(self, sDirectory = "", sUser = None, sPassword = None, fMode = 0, fFlags = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sDirectory = sDirectory;
        self.fMode = fMode;
        self.fFlags = fFlags;

class tdTestDirCreateTemp(tdTestGuestCtrlBase):
    """
    Test for the directoryCreateTemp call.
    """
    def __init__(self, sDirectory = "", sTemplate = "", sUser = None, sPassword = None, fMode = 0, fSecure = False):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sDirectory = sDirectory;
        self.sTemplate = sTemplate;
        self.fMode = fMode;
        self.fSecure = fSecure;

class tdTestDirOpen(tdTestGuestCtrlBase):
    """
    Test for the directoryOpen call.
    """
    def __init__(self, sDirectory = "", sUser = None, sPassword = None, sFilter = "", fFlags = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sDirectory = sDirectory;
        self.sFilter = sFilter;
        self.fFlags = fFlags or [];

class tdTestDirRead(tdTestDirOpen):
    """
    Test for the opening, reading and closing a certain directory.
    """
    def __init__(self, sDirectory = "", sUser = None, sPassword = None,
                 sFilter = "", fFlags = None):
        tdTestDirOpen.__init__(self, sDirectory, sUser, sPassword, sFilter, fFlags);

class tdTestExec(tdTestGuestCtrlBase):
    """
    Specifies exactly one guest control execution test.
    Has a default timeout of 5 minutes (for safety).
    """
    def __init__(self, sCmd = "", aArgs = None, aEnv = None, fFlags = None,             # pylint: disable=too-many-arguments
                 timeoutMS = 5 * 60 * 1000, sUser = None, sPassword = None, sDomain = None, fWaitForExit = True):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain);
        self.sCmd = sCmd;
        self.aArgs = aArgs if aArgs is not None else [sCmd,];
        self.aEnv = aEnv;
        self.fFlags = fFlags or [];
        self.timeoutMS = timeoutMS;
        self.fWaitForExit = fWaitForExit;
        self.uExitStatus = 0;
        self.iExitCode = 0;
        self.cbStdOut = 0;
        self.cbStdErr = 0;
        self.sBuf = '';

class tdTestFileExists(tdTestGuestCtrlBase):
    """
    Test for the file exists API call (fileExists).
    """
    def __init__(self, sFile = "", sUser = None, sPassword = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sFile = sFile;

class tdTestFileRemove(tdTestGuestCtrlBase):
    """
    Test querying guest file information.
    """
    def __init__(self, sFile = "", sUser = None, sPassword = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sFile = sFile;

class tdTestFileStat(tdTestGuestCtrlBase):
    """
    Test querying guest file information.
    """
    def __init__(self, sFile = "", sUser = None, sPassword = None, cbSize = 0, eFileType = 0):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sFile = sFile;
        self.cbSize = cbSize;
        self.eFileType = eFileType;

class tdTestFileIO(tdTestGuestCtrlBase):
    """
    Test for the IGuestFile object.
    """
    def __init__(self, sFile = "", sUser = None, sPassword = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sFile = sFile;

class tdTestFileQuerySize(tdTestGuestCtrlBase):
    """
    Test for the file size query API call (fileQuerySize).
    """
    def __init__(self, sFile = "", sUser = None, sPassword = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sFile = sFile;

class tdTestFileReadWrite(tdTestGuestCtrlBase):
    """
    Tests reading from guest files.
    """
    def __init__(self, sFile = "", sUser = None, sPassword = None, sOpenMode = "r", # pylint: disable=too-many-arguments
                 sDisposition = "", sSharingMode = "", fCreationMode = 0, offFile = 0, cbToReadWrite = 0, abBuf = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain = None);
        self.sFile = sFile;
        self.sOpenMode = sOpenMode;
        self.sDisposition = sDisposition;
        self.sSharingMode = sSharingMode;
        self.fCreationMode = fCreationMode;
        self.offFile = offFile;
        self.cbToReadWrite = cbToReadWrite;
        self.abBuf = abBuf;

    def getOpenAction(self):
        """ Converts string disposition to open action enum. """
        if self.sDisposition == 'oe': return vboxcon.FileOpenAction_OpenExisting;
        if self.sDisposition == 'oc': return vboxcon.FileOpenAction_OpenOrCreate;
        if self.sDisposition == 'ce': return vboxcon.FileOpenAction_CreateNew;
        if self.sDisposition == 'ca': return vboxcon.FileOpenAction_CreateOrReplace;
        if self.sDisposition == 'ot': return vboxcon.FileOpenAction_OpenExistingTruncated;
        if self.sDisposition == 'oa': return vboxcon.FileOpenAction_AppendOrCreate;
        raise base.GenError(self.sDisposition);

    def getAccessMode(self):
        """ Converts open mode to access mode enum. """
        if self.sOpenMode == 'r':  return vboxcon.FileAccessMode_ReadOnly;
        if self.sOpenMode == 'w':  return vboxcon.FileAccessMode_WriteOnly;
        if self.sOpenMode == 'w+': return vboxcon.FileAccessMode_ReadWrite;
        if self.sOpenMode == 'r+': return vboxcon.FileAccessMode_ReadWrite;
        raise base.GenError(self.sOpenMode);

    def getSharingMode(self):
        """ Converts the sharing mode. """
        return vboxcon.FileSharingMode_All;

class tdTestSession(tdTestGuestCtrlBase):
    """
    Test the guest session handling.
    """
    def __init__(self, sUser = None, sPassword = None, sDomain = None, sSessionName = ""):
        tdTestGuestCtrlBase.__init__(self);
        self.sSessionName = sSessionName;
        self.oCreds       = tdCtxCreds(sUser, sPassword, sDomain);

    def getSessionCount(self, oVBoxMgr):
        """
        Helper for returning the number of currently
        opened guest sessions of a VM.
        """
        if self.oTest.oGuest is None:
            return 0;
        try:
            aoSession = oVBoxMgr.getArray(self.oTest.oGuest, 'sessions')
        except:
            reporter.errorXcpt('sSessionName: %s' % (self.sSessionName,));
            return 0;
        return len(aoSession);

class tdTestSessionEx(tdTestGuestCtrlBase):
    """
    Test the guest session.
    """
    def __init__(self, aoSteps = None, enmUser = None):
        tdTestGuestCtrlBase.__init__(self);
        assert enmUser is None; # For later.
        self.enmUser = enmUser;
        self.aoSteps = aoSteps if aoSteps is not None else [];

    def execute(self, oTstDrv, oVmSession, oTxsSession, oTestVm, sMsgPrefix):
        """
        Executes the test.
        """
        #
        # Create a session.
        #
        assert self.enmUser is None; # For later.
        self.oCreds = tdCtxCreds();
        self.setEnvironment(oVmSession, oTxsSession, oTestVm);
        reporter.log2('%s: %s steps' % (sMsgPrefix, len(self.aoSteps),));
        fRc, oCurSession = self.createSession(sMsgPrefix);
        if fRc is True:
            #
            # Execute the tests.
            #
            try:
                fRc = self.executeSteps(oTstDrv, oCurSession, sMsgPrefix);
            except:
                fRc = reporter.errorXcpt('%s: Unexpected exception executing test steps' % (sMsgPrefix,));

            #
            # Close the session.
            #
            fRc2 = self.closeSession(True);
            if fRc2 is False:
                fRc = reporter.error('%s: Session could not be closed' % (sMsgPrefix,));
        else:
            fRc = reporter.error('%s: Session creation failed' % (sMsgPrefix,));
        return fRc;

    def executeSteps(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Executes just the steps.
        Returns True on success, False on test failure.
        """
        fRc = True;
        for (i, oStep) in enumerate(self.aoSteps):
            fRc2 = oStep.execute(oTstDrv, oGstCtrlSession, sMsgPrefix + ', step #%d' % i);
            if fRc2 is True:
                pass;
            elif fRc2 is None:
                reporter.log('%s: skipping remaining %d steps' % (sMsgPrefix, len(self.aoSteps) - i - 1,));
                break;
            else:
                fRc = False;
        return fRc;

    @staticmethod
    def executeListTestSessions(aoTests, oTstDrv, oVmSession, oTxsSession, oTestVm, sMsgPrefix):
        """
        Works thru a list of tdTestSessionEx object.
        """
        fRc = True;
        for (i, oCurTest) in enumerate(aoTests):
            try:
                fRc2 = oCurTest.execute(oTstDrv, oVmSession, oTxsSession, oTestVm, '%s / %#d' % (sMsgPrefix, i,));
                if fRc2 is not True:
                    fRc = False;
            except:
                fRc = reporter.errorXcpt('%s: Unexpected exception executing test #%d' % (sMsgPrefix, i ,));

        return (fRc, oTxsSession);


class tdSessionStepBase(object):
    """
    Base class for the guest control session test steps.
    """

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Executes the test step.

        Returns True on success.
        Returns False on failure (must be reported as error).
        Returns None if to skip the remaining steps.
        """
        _ = oTstDrv;
        _ = oGstCtrlSession;
        return reporter.error('%s: Missing execute implementation: %s' % (sMsgPrefix, self,));


class tdStepRequireMinimumApiVer(tdSessionStepBase):
    """
    Special test step which will cause executeSteps to skip the remaining step
    if the VBox API is too old:
    """
    def __init__(self, fpMinApiVer):
        self.fpMinApiVer = fpMinApiVer;

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """ Returns None if API version is too old, otherwise True. """
        if oTstDrv.fpApiVer >= self.fpMinApiVer:
            return True;
        _ = oGstCtrlSession;
        _ = sMsgPrefix;
        return None; # Special return value. Don't use elsewhere.


#
# Scheduling Environment Changes with the Guest Control Session.
#

class tdStepSessionSetEnv(tdSessionStepBase):
    """
    Guest session environment: schedule putenv
    """
    def __init__(self, sVar, sValue, hrcExpected = 0):
        self.sVar        = sVar;
        self.sValue      = sValue;
        self.hrcExpected = hrcExpected;

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Executes the step.
        Returns True on success, False on test failure.
        """
        reporter.log2('tdStepSessionSetEnv: sVar=%s sValue=%s hrcExpected=%#x' % (self.sVar, self.sValue, self.hrcExpected,));
        try:
            if oTstDrv.fpApiVer >= 5.0:
                oGstCtrlSession.environmentScheduleSet(self.sVar, self.sValue);
            else:
                oGstCtrlSession.environmentSet(self.sVar, self.sValue);
        except vbox.ComException as oXcpt:
            # Is this an expected failure?
            if vbox.ComError.equal(oXcpt, self.hrcExpected):
                return True;
            return reporter.errorXcpt('%s: Expected hrc=%#x (%s) got %#x (%s) instead (setenv %s=%s)'
                                      % (sMsgPrefix, self.hrcExpected, vbox.ComError.toString(self.hrcExpected),
                                         vbox.ComError.getXcptResult(oXcpt),
                                         vbox.ComError.toString(vbox.ComError.getXcptResult(oXcpt)),
                                         self.sVar, self.sValue,));
        except:
            return reporter.errorXcpt('%s: Unexpected exception in tdStepSessionSetEnv::execute (%s=%s)'
                                      % (sMsgPrefix, self.sVar, self.sValue,));

        # Should we succeed?
        if self.hrcExpected != 0:
            return reporter.error('%s: Expected hrcExpected=%#x, got S_OK (putenv %s=%s)'
                                  % (sMsgPrefix, self.hrcExpected, self.sVar, self.sValue,));
        return True;

class tdStepSessionUnsetEnv(tdSessionStepBase):
    """
    Guest session environment: schedule unset.
    """
    def __init__(self, sVar, hrcExpected = 0):
        self.sVar        = sVar;
        self.hrcExpected = hrcExpected;

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Executes the step.
        Returns True on success, False on test failure.
        """
        reporter.log2('tdStepSessionUnsetEnv: sVar=%s hrcExpected=%#x' % (self.sVar, self.hrcExpected,));
        try:
            if oTstDrv.fpApiVer >= 5.0:
                oGstCtrlSession.environmentScheduleUnset(self.sVar);
            else:
                oGstCtrlSession.environmentUnset(self.sVar);
        except vbox.ComException as oXcpt:
            # Is this an expected failure?
            if vbox.ComError.equal(oXcpt, self.hrcExpected):
                return True;
            return reporter.errorXcpt('%s: Expected hrc=%#x (%s) got %#x (%s) instead (unsetenv %s)'
                                       % (sMsgPrefix, self.hrcExpected, vbox.ComError.toString(self.hrcExpected),
                                          vbox.ComError.getXcptResult(oXcpt),
                                          vbox.ComError.toString(vbox.ComError.getXcptResult(oXcpt)),
                                          self.sVar,));
        except:
            return reporter.errorXcpt('%s: Unexpected exception in tdStepSessionUnsetEnv::execute (%s)'
                                      % (sMsgPrefix, self.sVar,));

        # Should we succeed?
        if self.hrcExpected != 0:
            return reporter.error('%s: Expected hrcExpected=%#x, got S_OK (unsetenv %s)'
                                  % (sMsgPrefix, self.hrcExpected, self.sVar,));
        return True;

class tdStepSessionBulkEnv(tdSessionStepBase):
    """
    Guest session environment: Bulk environment changes.
    """
    def __init__(self, asEnv = None, hrcExpected = 0):
        self.asEnv = asEnv if asEnv is not None else [];
        self.hrcExpected = hrcExpected;

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Executes the step.
        Returns True on success, False on test failure.
        """
        reporter.log2('tdStepSessionBulkEnv: asEnv=%s hrcExpected=%#x' % (self.asEnv, self.hrcExpected,));
        try:
            if oTstDrv.fpApiVer >= 5.0:
                oTstDrv.oVBoxMgr.setArray(oGstCtrlSession, 'environmentChanges', self.asEnv);
            else:
                oTstDrv.oVBoxMgr.setArray(oGstCtrlSession, 'environment', self.asEnv);
        except vbox.ComException as oXcpt:
            # Is this an expected failure?
            if vbox.ComError.equal(oXcpt, self.hrcExpected):
                return True;
            return reporter.errorXcpt('%s: Expected hrc=%#x (%s) got %#x (%s) instead (asEnv=%s)'
                                       % (sMsgPrefix, self.hrcExpected, vbox.ComError.toString(self.hrcExpected),
                                          vbox.ComError.getXcptResult(oXcpt),
                                          vbox.ComError.toString(vbox.ComError.getXcptResult(oXcpt)),
                                          self.asEnv,));
        except:
            return reporter.errorXcpt('%s: Unexpected exception writing the environmentChanges property (asEnv=%s).'
                                      % (sMsgPrefix, self.asEnv));
        return True;

class tdStepSessionClearEnv(tdStepSessionBulkEnv):
    """
    Guest session environment: clears the scheduled environment changes.
    """
    def __init__(self):
        tdStepSessionBulkEnv.__init__(self);


class tdStepSessionCheckEnv(tdSessionStepBase):
    """
    Check the currently scheduled environment changes of a guest control session.
    """
    def __init__(self, asEnv = None):
        self.asEnv = asEnv if asEnv is not None else [];

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Executes the step.
        Returns True on success, False on test failure.
        """
        reporter.log2('tdStepSessionCheckEnv: asEnv=%s' % (self.asEnv,));

        #
        # Get the environment change list.
        #
        try:
            if oTstDrv.fpApiVer >= 5.0:
                asCurEnv = oTstDrv.oVBoxMgr.getArray(oGstCtrlSession, 'environmentChanges');
            else:
                asCurEnv = oTstDrv.oVBoxMgr.getArray(oGstCtrlSession, 'environment');
        except:
            return reporter.errorXcpt('%s: Unexpected exception reading the environmentChanges property.' % (sMsgPrefix,));

        #
        # Compare it with the expected one by trying to remove each expected value
        # and the list anything unexpected.
        #
        fRc = True;
        asCopy = list(asCurEnv); # just in case asCurEnv is immutable
        for sExpected in self.asEnv:
            try:
                asCopy.remove(sExpected);
            except:
                fRc = reporter.error('%s: Expected "%s" to be in the resulting environment' % (sMsgPrefix, sExpected,));
        for sUnexpected in asCopy:
            fRc = reporter.error('%s: Unexpected "%s" in the resulting environment' % (sMsgPrefix, sUnexpected,));

        if fRc is not True:
            reporter.log2('%s: Current environment: %s' % (sMsgPrefix, asCurEnv));
        return fRc;


#
# File system object statistics (i.e. stat()).
#

class tdStepStat(tdSessionStepBase):
    """
    Stats a file system object.
    """
    def __init__(self, sPath, hrcExpected = 0, fFound = True, fFollowLinks = True, enmType = None):
        self.sPath        = sPath;
        self.hrcExpected  = hrcExpected;
        self.fFound       = fFound;
        self.fFollowLinks = fFollowLinks;
        self.enmType      = enmType if enmType is not None else vboxcon.FsObjType_File;
        self.cbExactSize  = None;
        self.cbMinSize    = None;

    def execute(self, oTstDrv, oGstCtrlSession, sMsgPrefix):
        """
        Execute the test step.
        """
        reporter.log2('tdStepStat: sPath=%s enmType=%s hrcExpected=%s fFound=%s fFollowLinks=%s'
                      % (self.sPath, self.enmType, self.hrcExpected, self.fFound, self.fFollowLinks,));

        # Don't execute non-file tests on older VBox version.
        if oTstDrv.fpApiVer >= 5.0 or self.enmType == vboxcon.FsObjType_File or not self.fFound:
            #
            # Call the API.
            #
            try:
                if oTstDrv.fpApiVer >= 5.0:
                    oFsInfo = oGstCtrlSession.fsObjQueryInfo(self.sPath, self.fFollowLinks);
                else:
                    oFsInfo = oGstCtrlSession.fileQueryInfo(self.sPath);
            except vbox.ComException as oXcpt:
                ## @todo: The error reporting in the API just plain sucks! Most of the errors are
                ##        VBOX_E_IPRT_ERROR and there seems to be no way to distinguish between
                ##        non-existing files/path and a lot of other errors.  Fix API and test!
                if not self.fFound:
                    return True;
                if vbox.ComError.equal(oXcpt, self.hrcExpected): # Is this an expected failure?
                    return True;
                return reporter.errorXcpt('%s: Unexpected exception for exiting path "%s" (enmType=%s, hrcExpected=%s):'
                                          % (sMsgPrefix, self.sPath, self.enmType, self.hrcExpected,));
            except:
                return reporter.errorXcpt('%s: Unexpected exception in tdStepStat::execute (%s)'
                                          % (sMsgPrefix, self.sPath,));
            if oFsInfo is None:
                return reporter.error('%s: "%s" got None instead of IFsObjInfo instance!' % (sMsgPrefix, self.sPath,));

            #
            # Check type expectations.
            #
            try:
                enmType = oFsInfo.type;
            except:
                return reporter.errorXcpt('%s: Unexpected exception in reading "IFsObjInfo::type"' % (sMsgPrefix,));
            if enmType != self.enmType:
                return reporter.error('%s: "%s" has type %s, expected %s'
                                      % (sMsgPrefix, self.sPath, enmType, self.enmType));

            #
            # Check size expectations.
            # Note! This is unicode string here on windows, for some reason.
            #       long long mapping perhaps?
            #
            try:
                cbObject = long(oFsInfo.objectSize);
            except:
                return reporter.errorXcpt('%s: Unexpected exception in reading "IFsObjInfo::objectSize"'
                                          % (sMsgPrefix,));
            if    self.cbExactSize is not None \
              and cbObject != self.cbExactSize:
                return reporter.error('%s: "%s" has size %s bytes, expected %s bytes'
                                      % (sMsgPrefix, self.sPath, cbObject, self.cbExactSize));
            if    self.cbMinSize is not None \
              and cbObject < self.cbMinSize:
                return reporter.error('%s: "%s" has size %s bytes, expected as least %s bytes'
                                      % (sMsgPrefix, self.sPath, cbObject, self.cbMinSize));
        return True;

class tdStepStatDir(tdStepStat):
    """ Checks for an existing directory. """
    def __init__(self, sDirPath):
        tdStepStat.__init__(self, sPath = sDirPath, enmType = vboxcon.FsObjType_Directory);

class tdStepStatFile(tdStepStat):
    """ Checks for an existing file  """
    def __init__(self, sFilePath):
        tdStepStat.__init__(self, sPath = sFilePath, enmType = vboxcon.FsObjType_File);

class tdStepStatFileSize(tdStepStat):
    """ Checks for an existing file of a given expected size.. """
    def __init__(self, sFilePath, cbExactSize = 0):
        tdStepStat.__init__(self, sPath = sFilePath, enmType = vboxcon.FsObjType_File);
        self.cbExactSize = cbExactSize;

class tdStepStatFileNotFound(tdStepStat):
    """ Checks for an existing directory. """
    def __init__(self, sPath):
        tdStepStat.__init__(self, sPath = sPath, fFound = False);

class tdStepStatPathNotFound(tdStepStat):
    """ Checks for an existing directory. """
    def __init__(self, sPath):
        tdStepStat.__init__(self, sPath = sPath, fFound = False);


#
#
#

class tdTestSessionFileRefs(tdTestGuestCtrlBase):
    """
    Tests session file (IGuestFile) reference counting.
    """
    def __init__(self, cRefs = 0):
        tdTestGuestCtrlBase.__init__(self);
        self.cRefs = cRefs;

class tdTestSessionDirRefs(tdTestGuestCtrlBase):
    """
    Tests session directory (IGuestDirectory) reference counting.
    """
    def __init__(self, cRefs = 0):
        tdTestGuestCtrlBase.__init__(self);
        self.cRefs = cRefs;

class tdTestSessionProcRefs(tdTestGuestCtrlBase):
    """
    Tests session process (IGuestProcess) reference counting.
    """
    def __init__(self, cRefs = 0):
        tdTestGuestCtrlBase.__init__(self);
        self.cRefs = cRefs;

class tdTestUpdateAdditions(tdTestGuestCtrlBase):
    """
    Test updating the Guest Additions inside the guest.
    """
    def __init__(self, sSrc = "", aArgs = None, fFlags = None,
                 sUser = None, sPassword = None, sDomain = None):
        tdTestGuestCtrlBase.__init__(self);
        self.oCreds = tdCtxCreds(sUser, sPassword, sDomain);
        self.sSrc = sSrc;
        self.aArgs = aArgs;
        self.fFlags = fFlags;

class tdTestResult(object):
    """
    Base class for test results.
    """
    def __init__(self, fRc = False):
        ## The overall test result.
        self.fRc = fRc;

class tdTestResultDirRead(tdTestResult):
    """
    Test result for reading guest directories.
    """
    def __init__(self, fRc = False, cFiles = 0, cDirs = 0, cOthers = None):
        tdTestResult.__init__(self, fRc = fRc);
        self.cFiles = cFiles;
        self.cDirs = cDirs;
        self.cOthers = cOthers;

class tdTestResultExec(tdTestResult):
    """
    Holds a guest process execution test result,
    including the exit code, status + fFlags.
    """
    def __init__(self, fRc = False, uExitStatus = 500, iExitCode = 0, sBuf = None, cbBuf = 0, cbStdOut = 0, cbStdErr = 0):
        tdTestResult.__init__(self);
        ## The overall test result.
        self.fRc = fRc;
        ## Process exit stuff.
        self.uExitStatus = uExitStatus;
        self.iExitCode = iExitCode;
        ## Desired buffer length returned back from stdout/stderr.
        self.cbBuf = cbBuf;
        ## Desired buffer result from stdout/stderr. Use with caution!
        self.sBuf = sBuf;
        self.cbStdOut = cbStdOut;
        self.cbStdErr = cbStdErr;

class tdTestResultFileStat(tdTestResult):
    """
    Test result for stat'ing guest files.
    """
    def __init__(self, fRc = False,
                 cbSize = 0, eFileType = 0):
        tdTestResult.__init__(self, fRc = fRc);
        self.cbSize = cbSize;
        self.eFileType = eFileType;
        ## @todo Add more information.

class tdTestResultFileReadWrite(tdTestResult):
    """
    Test result for reading + writing guest directories.
    """
    def __init__(self, fRc = False,
                 cbProcessed = 0, offFile = 0, abBuf = None):
        tdTestResult.__init__(self, fRc = fRc);
        self.cbProcessed = cbProcessed;
        self.offFile = offFile;
        self.abBuf = abBuf;

class tdTestResultSession(tdTestResult):
    """
    Test result for guest session counts.
    """
    def __init__(self, fRc = False, cNumSessions = 0):
        tdTestResult.__init__(self, fRc = fRc);
        self.cNumSessions = cNumSessions;

class SubTstDrvAddGuestCtrl(base.SubTestDriverBase):
    """
    Sub-test driver for executing guest control (VBoxService, IGuest) tests.
    """

    def __init__(self, oTstDrv):
        base.SubTestDriverBase.__init__(self, oTstDrv, 'add-guest-ctrl', 'Guest Control');

        ## @todo base.TestBase.
        self.asTestsDef = \
        [
            'session_basic', 'session_env', 'session_file_ref', 'session_dir_ref', 'session_proc_ref', 'session_reboot',
            'exec_basic', 'exec_errorlevel', 'exec_timeout',
            'dir_create', 'dir_create_temp', 'dir_read',
            'file_remove', 'file_stat', 'file_read', 'file_write',
            'copy_to', 'copy_from',
            'update_additions'
        ];
        self.asTests    = self.asTestsDef;
        self.asRsrcs    = ['5.3/guestctrl/50mb_rnd.dat', ];

    def parseOption(self, asArgs, iArg):                                        # pylint: disable=too-many-branches,too-many-statements
        if asArgs[iArg] == '--add-guest-ctrl-tests':
            iArg += 1;
            iNext = self.oTstDrv.requireMoreArgs(1, asArgs, iArg);
            if asArgs[iArg] == 'all': # Nice for debugging scripts.
                self.asTests = self.asTestsDef;
            else:
                self.asTests = asArgs[iArg].split(':');
                for s in self.asTests:
                    if s not in self.asTestsDef:
                        raise base.InvalidOption('The "--add-guest-ctrl-tests" value "%s" is not valid; valid values are: %s'
                                                 % (s, ' '.join(self.asTestsDef)));
            return iNext;
        return iArg;

    def showUsage(self):
        base.SubTestDriverBase.showUsage(self);
        reporter.log('  --add-guest-ctrl-tests  <s1[:s2[:]]>');
        reporter.log('      Default: %s  (all)' % (':'.join(self.asTestsDef)));
        return True;

    def testIt(self, oTestVm, oSession, oTxsSession):
        """
        Executes the test.

        Returns fRc, oTxsSession.  The latter may have changed.
        """
        reporter.log("Active tests: %s" % (self.asTests,));

        fRc = True;

        # Do the testing.
        reporter.testStart('Session Basics');
        fSkip = 'session_basic' not in self.asTests;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlSession(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Session Environment');
        fSkip = 'session_env' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlSessionEnvironment(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Session File References');
        fSkip = 'session_file_ref' not in self.asTests;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlSessionFileRefs(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        ## @todo Implement this.
        #reporter.testStart('Session Directory References');
        #fSkip = 'session_dir_ref' not in self.asTests;
        #if fSkip is False:
        #    fRc, oTxsSession = self.testGuestCtrlSessionDirRefs(oSession, oTxsSession, oTestVm);
        #reporter.testDone(fSkip);

        reporter.testStart('Session Process References');
        fSkip = 'session_proc_ref' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlSessionProcRefs(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Session w/ Guest Reboot');
        fSkip =    'session_reboot' not in self.asTests \
                or self.oTstDrv.fpApiVer <= 6.0; # Not backported yet.
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlSessionReboot(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Execution');
        fSkip = 'exec_basic' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlExec(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Execution Error Levels');
        fSkip = 'exec_errorlevel' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlExecErrorLevel(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Execution Timeouts');
        fSkip = 'exec_timeout' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlExecTimeout(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Creating directories');
        fSkip = 'dir_create' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlDirCreate(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Creating temporary directories');
        fSkip = 'dir_create_temp' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlDirCreateTemp(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Reading directories');
        fSkip = 'dir_read' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlDirRead(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Copy to guest');
        fSkip = 'copy_to' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlCopyTo(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Copy from guest');
        fSkip = 'copy_from' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlCopyFrom(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Removing files');
        fSkip = 'file_remove' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlFileRemove(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Querying file information (stat)');
        fSkip = 'file_stat' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlFileStat(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('File read');
        fSkip = 'file_read' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlFileRead(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('File write');
        fSkip = 'file_write' not in self.asTests or fRc is False;
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlFileWrite(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        reporter.testStart('Updating Guest Additions');
        fSkip = 'update_additions' not in self.asTests or fRc is False;
        # Skip test for updating Guest Additions if we run on a too old (Windows) guest.
        fSkip = oTestVm.sKind in ('WindowsNT4', 'Windows2000', 'WindowsXP', 'Windows2003');
        if fSkip is False:
            fRc, oTxsSession = self.testGuestCtrlUpdateAdditions(oSession, oTxsSession, oTestVm);
        reporter.testDone(fSkip);

        return (fRc, oTxsSession);

    @staticmethod
    def getGuestSystemDir(oTestVm):
        """
        Helper for finding a system directory in the test VM that we can play around with.

        On Windows this is always the System32 directory, so this function can be used as
        basis for locating other files in or under that directory.
        """
        if oTestVm.isWindows():
            if oTestVm.sKind in ['WindowsNT4', 'WindowsNT3x',]:
                return 'C:\\Winnt\\System32';
            return 'C:\\Windows\\System32';
        if oTestVm.isOS2():
            return 'C:\\OS2\\DLL';
        return "/bin";

    @staticmethod
    def getGuestSystemShell(oTestVm):
        """
        Helper for finding the default system shell in the test VM.
        """
        if oTestVm.isWindows():
            return SubTstDrvAddGuestCtrl.getGuestSystemDir(oTestVm) + '\\cmd.exe';
        if oTestVm.isOS2():
            return SubTstDrvAddGuestCtrl.getGuestSystemDir(oTestVm) + '\\..\\CMD.EXE';
        return "/bin/sh";

    @staticmethod
    def getGuestSystemFileForReading(oTestVm):
        """
        Helper for finding a file in the test VM that we can read.
        """
        if oTestVm.isWindows():
            return SubTstDrvAddGuestCtrl.getGuestSystemDir(oTestVm) + '\\ntdll.dll';
        if oTestVm.isOS2():
            return SubTstDrvAddGuestCtrl.getGuestSystemDir(oTestVm) + '\\DOSCALL1.DLL';
        return "/bin/sh";

    def gctrlCopyFileFrom(self, oGuestSession, sSrc, sDst, fFlags, fExpected):
        """
        Helper function to copy a single file from the guest to the host.
        """
        reporter.log2('Copying guest file "%s" to host "%s"' % (sSrc, sDst));
        try:
            if self.oTstDrv.fpApiVer >= 5.0:
                oCurProgress = oGuestSession.fileCopyFromGuest(sSrc, sDst, fFlags);
            else:
                oCurProgress = oGuestSession.copyFrom(sSrc, sDst, fFlags);
        except:
            reporter.maybeErrXcpt(fExpected, 'Copy from exception for sSrc="%s", sDst="%s":' % (sSrc, sDst,));
            return False;
        if oCurProgress is None:
            return reporter.error('No progress object returned');
        oProgress = vboxwrappers.ProgressWrapper(oCurProgress, self.oTstDrv.oVBoxMgr, self.oTstDrv, "gctrlFileCopyFrom");
        oProgress.wait();
        if not oProgress.isSuccess():
            oProgress.logResult(fIgnoreErrors = not fExpected);
            return False;
        return True;

    def gctrlCopyDirFrom(self, oGuestSession, sSrc, sDst, fFlags, fExpected):
        """
        Helper function to copy a directory from the guest to the host.
        """
        reporter.log2('Copying guest dir "%s" to host "%s"' % (sSrc, sDst));
        try:
            oCurProgress = oGuestSession.directoryCopyFromGuest(sSrc, sDst, fFlags);
        except:
            reporter.maybeErrXcpt(fExpected, 'Copy dir from exception for sSrc="%s", sDst="%s":' % (sSrc, sDst,));
            return False;
        if oCurProgress is None:
            return reporter.error('No progress object returned');

        oProgress = vboxwrappers.ProgressWrapper(oCurProgress, self.oTstDrv.oVBoxMgr, self.oTstDrv, "gctrlDirCopyFrom");
        oProgress.wait();
        if not oProgress.isSuccess():
            oProgress.logResult(fIgnoreErrors = not fExpected);
            return False;
        return True;

    def gctrlCopyFileTo(self, oGuestSession, sSrc, sDst, fFlags):
        """
        Helper function to copy a single file from host to the guest.
        """
        fRc = True; # Be optimistic.
        try:
            reporter.log2('Copying host file "%s" to guest "%s" (flags %s)' % (sSrc, sDst, fFlags));
            if self.oTstDrv.fpApiVer >= 5.0:
                oCurProgress = oGuestSession.fileCopyToGuest(sSrc, sDst, fFlags);
            else:
                oCurProgress = oGuestSession.copyTo(sSrc, sDst, fFlags);
            if oCurProgress is not None:
                oProgress = vboxwrappers.ProgressWrapper(oCurProgress, self.oTstDrv.oVBoxMgr, self.oTstDrv, "gctrlCopyFileTo");
                try:
                    oProgress.wait();
                    if not oProgress.isSuccess():
                        oProgress.logResult(fIgnoreErrors = True);
                        fRc = False;
                except:
                    reporter.logXcpt('Wait exception for sSrc="%s", sDst="%s":' % (sSrc, sDst));
                    fRc = False;
            else:
                reporter.error('No progress object returned');
                fRc = False;
        except:
            # Just log, don't assume an error here (will be done in the main loop then).
            reporter.logXcpt('Copy to exception for sSrc="%s", sDst="%s":' % (sSrc, sDst));
            fRc = False;

        return fRc;

    def gctrlCreateDir(self, oTest, oRes, oGuestSession):
        """
        Helper function to create a guest directory specified in
        the current test.
        """
        fRc = True; # Be optimistic.
        reporter.log2('Creating directory "%s"' % (oTest.sDirectory,));

        try:
            oGuestSession.directoryCreate(oTest.sDirectory, \
                                          oTest.fMode, oTest.fFlags);
            if self.oTstDrv.fpApiVer >= 5.0:
                fDirExists = oGuestSession.directoryExists(oTest.sDirectory, False);
            else:
                fDirExists = oGuestSession.directoryExists(oTest.sDirectory);
            if      fDirExists is False \
                and oRes.fRc is True:
                # Directory does not exist but we want it to.
                fRc = False;
        except:
            reporter.logXcpt('Directory create exception for directory "%s":' % (oTest.sDirectory,));
            if oRes.fRc is True:
                # Just log, don't assume an error here (will be done in the main loop then).
                fRc = False;
            # Directory creation failed, which was the expected result.

        return fRc;


    def gctrlReadDirTree(self, oTest, oGuestSession, fIsError, sSubDir = None):
        """
        Helper function to recursively read a guest directory tree specified in the current test.
        """
        sDir     = oTest.sDirectory;
        sFilter  = oTest.sFilter;
        fFlags   = oTest.fFlags;
        oTestVm  = oTest.oCreds.oTestVm;
        sCurDir  = oTestVm.pathJoinEx(sDir, sSubDir) if sSubDir else sDir;

        fRc      = True; # Be optimistic.
        cDirs    = 0;    # Number of directories read.
        cFiles   = 0;    # Number of files read.
        cOthers  = 0;    # Other files.

        ##
        ## @todo r=bird: Unlike fileOpen, directoryOpen will not fail if the directory does not exist.
        ##       This is of course a bug in the implementation, as it is documented to return
        ##       VBOX_E_OBJECT_NOT_FOUND or VBOX_E_IPRT_ERROR!
        ##

        # Open the directory:
        #reporter.log2('Directory="%s", filter="%s", fFlags="%s"' % (sCurDir, sFilter, fFlags));
        try:
            oCurDir = oGuestSession.directoryOpen(sCurDir, sFilter, fFlags);
        except:
            reporter.maybeErrXcpt(fIsError, 'sCurDir=%s sFilter=%s fFlags=%s' % (sCurDir, sFilter, fFlags,))
            return (False, 0, 0, 0);

        # Read the directory.
        while fRc is True:
            try:
                oFsObjInfo = oCurDir.read();
            except Exception as oXcpt:
                if vbox.ComError.notEqual(oXcpt, vbox.ComError.VBOX_E_OBJECT_NOT_FOUND):
                    ##
                    ## @todo r=bird: Change this to reporter.errorXcpt() once directoryOpen() starts
                    ##       working the way it is documented.
                    ##
                    reporter.maybeErrXcpt(fIsError, 'Error reading directory "%s":' % (sCurDir,)); # See above why 'maybe'.
                    fRc = False;
                #else: reporter.log2('\tNo more directory entries for "%s"' % (sCurDir,));
                break;

            try:
                sName = oFsObjInfo.name;
                eType = oFsObjInfo.type;
            except:
                fRc = reporter.errorXcpt();
                break;

            if sName in ('.', '..', ):
                if eType != vboxcon.FsObjType_Directory:
                    fRc = reporter.error('Wrong type for "%s": %d, expected %d (Directory)'
                                         % (sName, eType, vboxcon.FsObjType_Directory));
            elif eType == vboxcon.FsObjType_Directory:
                #reporter.log2('  Directory "%s"' % oFsObjInfo.name);
                aSubResult = self.gctrlReadDirTree(oTest, oGuestSession, fIsError,
                                                   oTestVm.pathJoinEx(sSubDir, sName) if sSubDir else sName);
                fRc      = aSubResult[0];
                cDirs   += aSubResult[1] + 1;
                cFiles  += aSubResult[2];
                cOthers += aSubResult[3];
            elif eType is vboxcon.FsObjType_File:
                #reporter.log2('  File "%s"' % oFsObjInfo.name);
                cFiles += 1;
            elif eType is vboxcon.FsObjType_Symlink:
                 #reporter.log2('  Symlink "%s" -- not tested yet' % oFsObjInfo.name);
                cOthers += 1;
            elif    oTestVm.isWindows() \
                 or oTestVm.isOS2() \
                 or eType not in (vboxcon.FsObjType_Fifo, vboxcon.FsObjType_DevChar, vboxcon.FsObjType_DevBlock,
                                  vboxcon.FsObjType_Socket, vboxcon.FsObjType_WhiteOut):
                fRc = reporter.error('Directory "%s" contains invalid directory entry "%s" (type %d)' %
                                     (sCurDir, oFsObjInfo.name, oFsObjInfo.type,));
            else:
                cOthers += 1;

        # Close the directory
        try:
            oCurDir.close();
        except:
            fRc = reporter.errorXcpt('sCurDir=%s' % (sCurDir));

        return (fRc, cDirs, cFiles, cOthers);

    def gctrlExecDoTest(self, i, oTest, oRes, oGuestSession):
        """
        Wrapper function around gctrlExecute to provide more sanity checking
        when needed in actual execution tests.
        """
        reporter.log('Testing #%d, cmd="%s" ...' % (i, oTest.sCmd));
        fRc = self.gctrlExecute(oTest, oGuestSession);
        if fRc is oRes.fRc:
            if fRc is True:
                # Compare exit status / code on successful process execution.
                if     oTest.uExitStatus != oRes.uExitStatus \
                    or oTest.iExitCode   != oRes.iExitCode:
                    reporter.error('Test #%d failed: Got exit status + code %d,%d, expected %d,%d'
                                   % (i, oTest.uExitStatus, oTest.iExitCode, oRes.uExitStatus,  oRes.iExitCode));
                    return False;
            if fRc is True:
                # Compare test / result buffers on successful process execution.
                if      oTest.sBuf is not None \
                    and oRes.sBuf is not None:
                    if bytes(oTest.sBuf) != bytes(oRes.sBuf):
                        reporter.error('Test #%d failed: Got buffer\n%s (%d bytes), expected\n%s (%d bytes)'
                                       % (i, map(hex, map(ord, oTest.sBuf)), len(oTest.sBuf), \
                                          map(hex, map(ord, oRes.sBuf)), len(oRes.sBuf)));
                        return False;
                    reporter.log2('Test #%d passed: Buffers match (%d bytes)' % (i, len(oRes.sBuf)));
                elif     oRes.sBuf is not None \
                     and oRes.sBuf:
                    reporter.error('Test #%d failed: Got no buffer data, expected\n%s (%dbytes)' %
                                   (i, map(hex, map(ord, oRes.sBuf)), len(oRes.sBuf)));
                    return False;
                elif     oRes.cbStdOut > 0 \
                     and oRes.cbStdOut != oTest.cbStdOut:
                    reporter.error('Test #%d failed: Got %d stdout data, expected %d'
                                   % (i, oTest.cbStdOut, oRes.cbStdOut));
                    return False;
        else:
            reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc, oRes.fRc));
            return False;
        return True;

    def gctrlExecute(self, oTest, oGuestSession):
        """
        Helper function to execute a program on a guest, specified in
        the current test.
        """
        fRc = True; # Be optimistic.

        ## @todo Compare execution timeouts!
        #tsStart = base.timestampMilli();

        reporter.log2('Using session user=%s, sDomain=%s, name=%s, timeout=%d'
                      % (oGuestSession.user, oGuestSession.domain,  oGuestSession.name, oGuestSession.timeout));
        reporter.log2('Executing sCmd=%s, fFlags=%s, timeoutMS=%d, aArgs=%s, aEnv=%s'
                      % (oTest.sCmd, oTest.fFlags, oTest.timeoutMS, oTest.aArgs, oTest.aEnv));
        try:
            curProc = oGuestSession.processCreate(oTest.sCmd,
                                                  oTest.aArgs if self.oTstDrv.fpApiVer >= 5.0 else oTest.aArgs[1:],
                                                  oTest.aEnv, oTest.fFlags, oTest.timeoutMS);
            if curProc is not None:
                reporter.log2('Process start requested, waiting for start (%dms) ...' % (oTest.timeoutMS,));
                aeWaitFor = [ vboxcon.ProcessWaitForFlag_Start ];
                waitResult = curProc.waitForArray(aeWaitFor, oTest.timeoutMS);
                reporter.log2('Wait result returned: %d, current process status is: %d' % (waitResult, curProc.status));

                if curProc.status == vboxcon.ProcessStatus_Started:
                    aeWaitFor = [ vboxcon.ProcessWaitForFlag_Terminate ];
                    if vboxcon.ProcessCreateFlag_WaitForStdOut in oTest.fFlags:
                        aeWaitFor.append(vboxcon.ProcessWaitForFlag_StdOut);
                    if vboxcon.ProcessCreateFlag_WaitForStdErr in oTest.fFlags:
                        aeWaitFor.append(vboxcon.ProcessWaitForFlag_StdErr);
                    ## @todo Add vboxcon.ProcessWaitForFlag_StdIn.
                    reporter.log2('Process (PID %d) started, waiting for termination (%dms), waitFlags=%s ...'
                                  % (curProc.PID, oTest.timeoutMS, aeWaitFor));
                    while True:
                        waitResult = curProc.waitForArray(aeWaitFor, oTest.timeoutMS);
                        reporter.log2('Wait returned: %d' % (waitResult,));
                        try:
                            # Try stdout.
                            if waitResult in (vboxcon.ProcessWaitResult_StdOut, vboxcon.ProcessWaitResult_WaitFlagNotSupported):
                                reporter.log2('Reading stdout ...');
                                abBuf = curProc.Read(1, 64 * 1024, oTest.timeoutMS);
                                if abBuf:
                                    reporter.log2('Process (PID %d) got %d bytes of stdout data' % (curProc.PID, len(abBuf)));
                                    oTest.cbStdOut += len(abBuf);
                                    oTest.sBuf = abBuf; # Appending does *not* work atm, so just assign it. No time now.
                            # Try stderr.
                            if waitResult in (vboxcon.ProcessWaitResult_StdErr, vboxcon.ProcessWaitResult_WaitFlagNotSupported):
                                reporter.log2('Reading stderr ...');
                                abBuf = curProc.Read(2, 64 * 1024, oTest.timeoutMS);
                                if abBuf:
                                    reporter.log2('Process (PID %d) got %d bytes of stderr data' % (curProc.PID, len(abBuf)));
                                    oTest.cbStdErr += len(abBuf);
                                    oTest.sBuf = abBuf; # Appending does *not* work atm, so just assign it. No time now.
                            # Use stdin.
                            if waitResult in (vboxcon.ProcessWaitResult_StdIn, vboxcon.ProcessWaitResult_WaitFlagNotSupported):
                                pass; #reporter.log2('Process (PID %d) needs stdin data' % (curProc.pid,));
                            # Termination or error?
                            if waitResult in (vboxcon.ProcessWaitResult_Terminate,
                                              vboxcon.ProcessWaitResult_Error,
                                              vboxcon.ProcessWaitResult_Timeout,):
                                reporter.log2('Process (PID %d) reported terminate/error/timeout: %d, status: %d'
                                              % (curProc.PID, waitResult, curProc.status));
                                break;
                        except:
                            # Just skip reads which returned nothing.
                            pass;
                    reporter.log2('Final process status (PID %d) is: %d' % (curProc.PID, curProc.status));
                    reporter.log2('Process (PID %d) %d stdout, %d stderr' % (curProc.PID, oTest.cbStdOut, oTest.cbStdErr));
            oTest.uExitStatus = curProc.status;
            oTest.iExitCode = curProc.exitCode;
            reporter.log2('Process (PID %d) has exit code: %d' % (curProc.PID, oTest.iExitCode));
        except KeyboardInterrupt:
            reporter.error('Process (PID %d) execution interrupted' % (curProc.PID,));
            if curProc is not None:
                curProc.close();
        except:
            # Just log, don't assume an error here (will be done in the main loop then).
            reporter.logXcpt('Execution exception for command "%s":' % (oTest.sCmd,));
            fRc = False;

        return fRc;

    def testGuestCtrlSessionEnvironment(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests the guest session environment changes.
        """
        aoTests = [
            # Check basic operations.
            tdTestSessionEx([ # Initial environment is empty.
                tdStepSessionCheckEnv(),
                # Check clearing empty env.
                tdStepSessionClearEnv(),
                tdStepSessionCheckEnv(),
                # Check set.
                tdStepSessionSetEnv('FOO', 'BAR'),
                tdStepSessionCheckEnv(['FOO=BAR',]),
                tdStepRequireMinimumApiVer(5.0), # 4.3 can't cope with the remainder.
                tdStepSessionClearEnv(),
                tdStepSessionCheckEnv(),
                # Check unset.
                tdStepSessionUnsetEnv('BAR'),
                tdStepSessionCheckEnv(['BAR']),
                tdStepSessionClearEnv(),
                tdStepSessionCheckEnv(),
                # Set + unset.
                tdStepSessionSetEnv('FOO', 'BAR'),
                tdStepSessionCheckEnv(['FOO=BAR',]),
                tdStepSessionUnsetEnv('FOO'),
                tdStepSessionCheckEnv(['FOO']),
                # Bulk environment changes (via attrib) (shall replace existing 'FOO').
                tdStepSessionBulkEnv( ['PATH=/bin:/usr/bin', 'TMPDIR=/var/tmp', 'USER=root']),
                tdStepSessionCheckEnv(['PATH=/bin:/usr/bin', 'TMPDIR=/var/tmp', 'USER=root']),
                ]),
            tdTestSessionEx([ # Check that setting the same value several times works.
                tdStepSessionSetEnv('FOO','BAR'),
                tdStepSessionCheckEnv([ 'FOO=BAR',]),
                tdStepSessionSetEnv('FOO','BAR2'),
                tdStepSessionCheckEnv([ 'FOO=BAR2',]),
                tdStepSessionSetEnv('FOO','BAR3'),
                tdStepSessionCheckEnv([ 'FOO=BAR3',]),
                tdStepRequireMinimumApiVer(5.0), # 4.3 can't cope with the remainder.
                # Add a little unsetting to the mix.
                tdStepSessionSetEnv('BAR', 'BEAR'),
                tdStepSessionCheckEnv([ 'FOO=BAR3', 'BAR=BEAR',]),
                tdStepSessionUnsetEnv('FOO'),
                tdStepSessionCheckEnv([ 'FOO', 'BAR=BEAR',]),
                tdStepSessionSetEnv('FOO','BAR4'),
                tdStepSessionCheckEnv([ 'FOO=BAR4', 'BAR=BEAR',]),
                # The environment is case sensitive.
                tdStepSessionSetEnv('foo','BAR5'),
                tdStepSessionCheckEnv([ 'FOO=BAR4', 'BAR=BEAR', 'foo=BAR5']),
                tdStepSessionUnsetEnv('foo'),
                tdStepSessionCheckEnv([ 'FOO=BAR4', 'BAR=BEAR', 'foo']),
                ]),
            tdTestSessionEx([ # Bulk settings merges stuff, last entry standing.
                tdStepSessionBulkEnv(['FOO=bar', 'foo=bar', 'FOO=doofus', 'TMPDIR=/tmp', 'foo=bar2']),
                tdStepSessionCheckEnv(['FOO=doofus', 'TMPDIR=/tmp', 'foo=bar2']),
                tdStepRequireMinimumApiVer(5.0), # 4.3 is buggy!
                tdStepSessionBulkEnv(['2=1+1', 'FOO=doofus2', ]),
                tdStepSessionCheckEnv(['2=1+1', 'FOO=doofus2' ]),
                ]),
            # Invalid variable names.
            tdTestSessionEx([
                tdStepSessionSetEnv('', 'FOO', vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                tdStepRequireMinimumApiVer(5.0), # 4.3 is too relaxed checking input!
                tdStepSessionSetEnv('=', '===', vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                tdStepSessionSetEnv('FOO=', 'BAR', vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                tdStepSessionSetEnv('=FOO', 'BAR', vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                tdStepRequireMinimumApiVer(5.0), # 4.3 is buggy and too relaxed!
                tdStepSessionBulkEnv(['', 'foo=bar'], vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                tdStepSessionBulkEnv(['=', 'foo=bar'], vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                tdStepSessionBulkEnv(['=FOO', 'foo=bar'], vbox.ComError.E_INVALIDARG),
                tdStepSessionCheckEnv(),
                ]),
            # A bit more weird keys/values.
            tdTestSessionEx([ tdStepSessionSetEnv('$$$', ''),
                              tdStepSessionCheckEnv([ '$$$=',]), ]),
            tdTestSessionEx([ tdStepSessionSetEnv('$$$', '%%%'),
                              tdStepSessionCheckEnv([ '$$$=%%%',]),
                            ]),
            tdTestSessionEx([ tdStepRequireMinimumApiVer(5.0), # 4.3 is buggy!
                              tdStepSessionSetEnv(u'ß$%ß&', ''),
                              tdStepSessionCheckEnv([ u'ß$%ß&=',]),
                            ]),
            # Misc stuff.
            tdTestSessionEx([ tdStepSessionSetEnv('FOO', ''),
                              tdStepSessionCheckEnv(['FOO=',]),
                            ]),
            tdTestSessionEx([ tdStepSessionSetEnv('FOO', 'BAR'),
                              tdStepSessionCheckEnv(['FOO=BAR',])
                            ],),
            tdTestSessionEx([ tdStepSessionSetEnv('FOO', 'BAR'),
                              tdStepSessionSetEnv('BAR', 'BAZ'),
                              tdStepSessionCheckEnv([ 'FOO=BAR', 'BAR=BAZ',]),
                            ]),
        ];
        return tdTestSessionEx.executeListTestSessions(aoTests, self.oTstDrv, oSession, oTxsSession, oTestVm, 'SessionEnv');

    def testGuestCtrlSession(self, oSession, oTxsSession, oTestVm):
        """
        Tests the guest session handling.
        """

        #
        # Parameters.
        #
        atTests = [
            # Invalid parameters.
            [ tdTestSession(sUser = ''), tdTestResultSession() ],
            [ tdTestSession(sPassword = 'bar'), tdTestResultSession() ],
            [ tdTestSession(sDomain = 'boo'),tdTestResultSession() ],
            [ tdTestSession(sPassword = 'bar', sDomain = 'boo'), tdTestResultSession() ],
            # User account without a passwort - forbidden.
            [ tdTestSession(sPassword = "" ), tdTestResultSession() ],
            # Wrong credentials.
            # Note: On Guest Additions < 4.3 this always succeeds because these don't
            #       support creating dedicated sessions. Instead, guest process creation
            #       then will fail. See note below.
            [ tdTestSession(sUser = 'foo', sPassword = 'bar', sDomain = 'boo'), tdTestResultSession() ],
            # Correct credentials.
            [ tdTestSession(), tdTestResultSession(fRc = True, cNumSessions = 1) ]
        ];

        fRc = True;
        for (i, tTest) in enumerate(atTests):
            oCurTest = tTest[0] # type: tdTestSession
            oCurRes  = tTest[1] # type: tdTestResult

            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            reporter.log('Testing #%d, user="%s", sPassword="%s", sDomain="%s" ...'
                         % (i, oCurTest.oCreds.sUser, oCurTest.oCreds.sPassword, oCurTest.oCreds.sDomain));
            sCurGuestSessionName = 'testGuestCtrlSession: Test #%d' % (i,);
            fRc2, oCurGuestSession = oCurTest.createSession(sCurGuestSessionName, fIsError = oCurRes.fRc);

            # See note about < 4.3 Guest Additions above.
            uProtocolVersion = 2;
            if oCurGuestSession is not None:
                try:
                    uProtocolVersion = oCurGuestSession.protocolVersion;
                except:
                    fRc = reporter.errorXcpt('Test #%d' % (i,));

            if uProtocolVersion >= 2 and fRc2 is not oCurRes.fRc:
                fRc = reporter.error('Test #%d failed: Session creation failed: Got %s, expected %s' % (i, fRc2, oCurRes.fRc,));

            if fRc2 and oCurGuestSession is None:
                fRc = reporter.error('Test #%d failed: no session object' % (i,));
                fRc2 = False;

            if fRc2:
                if uProtocolVersion >= 2: # For Guest Additions < 4.3 getSessionCount() always will return 1.
                    cCurSessions = oCurTest.getSessionCount(self.oTstDrv.oVBoxMgr);
                    if cCurSessions != oCurRes.cNumSessions:
                        fRc = reporter.error('Test #%d failed: Session count does not match: Got %d, expected %d'
                                             % (i, cCurSessions, oCurRes.cNumSessions));
                try:
                    sObjName = oCurGuestSession.name;
                except:
                    fRc = reporter.errorXcpt('Test #%d' % (i,));
                else:
                    if sObjName != sCurGuestSessionName:
                        fRc = reporter.error('Test #%d failed: Session name does not match: Got "%s", expected "%s"'
                                             % (i, sObjName, sCurGuestSessionName));
            fRc2 = oCurTest.closeSession(True);
            if fRc2 is False:
                fRc = reporter.error('Test #%d failed: Session could not be closed' % (i,));

        if fRc is False:
            return (False, oTxsSession);

        #
        # Multiple sessions.
        #
        cMaxGuestSessions = 31; # Maximum number of concurrent guest session allowed.
                                # Actually, this is 32, but we don't test session 0.
        aoMultiSessions = {};
        reporter.log2('Opening multiple guest tsessions at once ...');
        for i in xrange(cMaxGuestSessions + 1):
            aoMultiSessions[i] = tdTestSession(sSessionName = 'MultiSession #%d' % (i,));
            aoMultiSessions[i].setEnvironment(oSession, oTxsSession, oTestVm);

            cCurSessions = aoMultiSessions[i].getSessionCount(self.oTstDrv.oVBoxMgr);
            reporter.log2('MultiSession test #%d count is %d' % (i, cCurSessions));
            if cCurSessions != i:
                return (reporter.error('MultiSession count is %d, expected %d' % (cCurSessions, i)), oTxsSession);
            fRc2, _ = aoMultiSessions[i].createSession('MultiSession #%d' % (i,), i < cMaxGuestSessions);
            if fRc2 is not True:
                if i < cMaxGuestSessions:
                    return (reporter.error('MultiSession #%d test failed' % (i,)), oTxsSession);
                reporter.log('MultiSession #%d exceeded concurrent guest session count, good' % (i,));
                break;

        cCurSessions = aoMultiSessions[i].getSessionCount(self.oTstDrv.oVBoxMgr);
        if cCurSessions is not cMaxGuestSessions:
            return (reporter.error('Final session count %d, expected %d ' % (cCurSessions, cMaxGuestSessions,)), oTxsSession);

        reporter.log2('Closing MultiSessions ...');
        for i in xrange(cMaxGuestSessions):
            # Close this session:
            oClosedGuestSession = aoMultiSessions[i].oGuestSession;
            fRc2 = aoMultiSessions[i].closeSession(True);
            cCurSessions = aoMultiSessions[i].getSessionCount(self.oTstDrv.oVBoxMgr)
            reporter.log2('MultiSession #%d count is %d' % (i, cCurSessions,));
            if fRc2 is False:
                fRc = reporter.error('Closing MultiSession #%d failed' % (i,));
            elif cCurSessions != cMaxGuestSessions - (i + 1):
                fRc = reporter.error('Expected %d session after closing #%d, got %d instead'
                                     % (cMaxGuestSessions - (i + 1), cCurSessions, i,));
            assert aoMultiSessions[i].oGuestSession is None or not fRc2;
            ## @todo any way to check that the session is closed other than the 'sessions' attribute?

            # Try check that none of the remaining sessions got closed.
            try:
                aoGuestSessions = self.oTstDrv.oVBoxMgr.getArray(atTests[0][0].oTest.oGuest, 'sessions');
            except:
                return (reporter.errorXcpt('i=%d/%d' % (i, cMaxGuestSessions,)), oTxsSession);
            if oClosedGuestSession in aoGuestSessions:
                fRc = reporter.error('i=%d/%d: %s should not be in %s'
                                     % (i, cMaxGuestSessions, oClosedGuestSession, aoGuestSessions));
            if i + 1 < cMaxGuestSessions: # Not sure what xrange(2,2) does...
                for j in xrange(i + 1, cMaxGuestSessions):
                    if aoMultiSessions[j].oGuestSession not in aoGuestSessions:
                        fRc = reporter.error('i=%d/j=%d/%d: %s should be in %s'
                                             % (i, j, cMaxGuestSessions, aoMultiSessions[j].oGuestSession, aoGuestSessions));
                    ## @todo any way to check that they work?

        ## @todo Test session timeouts.

        return (fRc, oTxsSession);

    def testGuestCtrlSessionFileRefs(self, oSession, oTxsSession, oTestVm):
        """
        Tests the guest session file reference handling.
        """

        # Find a file to play around with:
        sFile = self.getGuestSystemFileForReading(oTestVm);

        # Use credential defaults.
        oCreds = tdCtxCreds();
        oCreds.applyDefaultsIfNotSet(oTestVm);

        # Number of stale guest files to create.
        cStaleFiles = 10;

        #
        # Start a session.
        #
        aeWaitFor = [ vboxcon.GuestSessionWaitForFlag_Start ];
        try:
            oGuest        = oSession.o.console.guest;
            oGuestSession = oGuest.createSession(oCreds.sUser, oCreds.sPassword, oCreds.sDomain, "testGuestCtrlSessionFileRefs");
            eWaitResult   = oGuestSession.waitForArray(aeWaitFor, 30 * 1000);
        except:
            return (reporter.errorXcpt(), oTxsSession);

        # Be nice to Guest Additions < 4.3: They don't support session handling and therefore return WaitFlagNotSupported.
        if eWaitResult not in (vboxcon.GuestSessionWaitResult_Start, vboxcon.GuestSessionWaitResult_WaitFlagNotSupported):
            return (reporter.error('Session did not start successfully - wait error: %d' % (eWaitResult,)), oTxsSession);
        reporter.log('Session successfully started');

        #
        # Open guest files and "forget" them (stale entries).
        # For them we don't have any references anymore intentionally.
        #
        reporter.log2('Opening stale files');
        fRc = True;
        for i in xrange(0, cStaleFiles):
            try:
                if self.oTstDrv.fpApiVer >= 5.0:
                    oGuestSession.fileOpen(sFile, vboxcon.FileAccessMode_ReadOnly, vboxcon.FileOpenAction_OpenExisting, 0);
                else:
                    oGuestSession.fileOpen(sFile, "r", "oe", 0);
                # Note: Use a timeout in the call above for not letting the stale processes
                #       hanging around forever.  This can happen if the installed Guest Additions
                #       do not support terminating guest processes.
            except:
                fRc = reporter.errorXcpt('Opening stale file #%d failed:' % (i,));
                break;

        try:    cFiles = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'files'));
        except: fRc = reporter.errorXcpt();
        else:
            if cFiles != cStaleFiles:
                fRc = reporter.error('Got %d stale files, expected %d' % (cFiles, cStaleFiles));

        if fRc is True:
            #
            # Open non-stale files and close them again.
            #
            reporter.log2('Opening non-stale files');
            aoFiles = [];
            for i in xrange(0, cStaleFiles):
                try:
                    if self.oTstDrv.fpApiVer >= 5.0:
                        oCurFile = oGuestSession.fileOpen(sFile, vboxcon.FileAccessMode_ReadOnly,
                                                          vboxcon.FileOpenAction_OpenExisting, 0);
                    else:
                        oCurFile = oGuestSession.fileOpen(sFile, "r", "oe", 0);
                    aoFiles.append(oCurFile);
                except:
                    fRc = reporter.errorXcpt('Opening non-stale file #%d failed:' % (i,));
                    break;

            # Check the count.
            try:    cFiles = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'files'));
            except: fRc = reporter.errorXcpt();
            else:
                if cFiles != cStaleFiles * 2:
                    fRc = reporter.error('Got %d total files, expected %d' % (cFiles, cStaleFiles * 2));

            # Close them.
            reporter.log2('Closing all non-stale files again ...');
            for i, oFile in enumerate(aoFiles):
                try:
                    oFile.close();
                except:
                    fRc = reporter.errorXcpt('Closing non-stale file #%d failed:' % (i,));

            # Check the count again.
            try:    cFiles = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'files'));
            except: fRc = reporter.errorXcpt();
            # Here we count the stale files (that is, files we don't have a reference
            # anymore for) and the opened and then closed non-stale files (that we still keep
            # a reference in aoFiles[] for).
            if cFiles != cStaleFiles:
                fRc = reporter.error('Got %d total files, expected %d' % (cFiles, cStaleFiles));

            #
            # Check that all (referenced) non-stale files are now in the "closed" state.
            #
            reporter.log2('Checking statuses of all non-stale files ...');
            for i, oFile in enumerate(aoFiles):
                try:
                    eFileStatus = aoFiles[i].status;
                except:
                    fRc = reporter.errorXcpt('Checking status of file #%d failed:' % (i,));
                else:
                    if eFileStatus != vboxcon.FileStatus_Closed:
                        fRc = reporter.error('Non-stale file #%d has status %d, expected %d'
                                             % (i, eFileStatus, vboxcon.FileStatus_Closed));

        if fRc is True:
            reporter.log2('All non-stale files closed');

        try:    cFiles = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'files'));
        except: fRc = reporter.errorXcpt();
        else:   reporter.log2('Final guest session file count: %d' % (cFiles,));

        #
        # Now try to close the session and see what happens.
        # Note! Session closing is why we've been doing all the 'if fRc is True' stuff above rather than returning.
        #
        reporter.log2('Closing guest session ...');
        try:
            oGuestSession.close();
        except:
            fRc = reporter.errorXcpt('Testing for stale processes failed:');

        return (fRc, oTxsSession);

    #def testGuestCtrlSessionDirRefs(self, oSession, oTxsSession, oTestVm):
    #    """
    #    Tests the guest session directory reference handling.
    #    """

    #    fRc = True;
    #    return (fRc, oTxsSession);

    def testGuestCtrlSessionProcRefs(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests the guest session process reference handling.
        """

        sCmd = self.getGuestSystemShell(oTestVm);
        aArgs = [sCmd,];

        # Use credential defaults.
        oCreds = tdCtxCreds();
        oCreds.applyDefaultsIfNotSet(oTestVm);

        # Number of stale guest processes to create.
        cStaleProcs = 10;

        #
        # Start a session.
        #
        aeWaitFor = [ vboxcon.GuestSessionWaitForFlag_Start ];
        try:
            oGuest        = oSession.o.console.guest;
            oGuestSession = oGuest.createSession(oCreds.sUser, oCreds.sPassword, oCreds.sDomain, "testGuestCtrlSessionFileRefs");
            eWaitResult   = oGuestSession.waitForArray(aeWaitFor, 30 * 1000);
        except:
            return (reporter.errorXcpt(), oTxsSession);

        # Be nice to Guest Additions < 4.3: They don't support session handling and therefore return WaitFlagNotSupported.
        if eWaitResult not in (vboxcon.GuestSessionWaitResult_Start, vboxcon.GuestSessionWaitResult_WaitFlagNotSupported):
            return (reporter.error('Session did not start successfully - wait error: %d' % (eWaitResult,)), oTxsSession);
        reporter.log('Session successfully started');

        #
        # Fire off forever-running processes and "forget" them (stale entries).
        # For them we don't have any references anymore intentionally.
        #
        reporter.log2('Starting stale processes...');
        fRc = True;
        for i in xrange(0, cStaleProcs):
            try:
                oGuestSession.processCreate(sCmd,
                                            aArgs if self.oTstDrv.fpApiVer >= 5.0 else aArgs[1:], [],
                                            [ vboxcon.ProcessCreateFlag_WaitForStdOut ], \
                                            30 * 1000);
                # Note: Use a timeout in the call above for not letting the stale processes
                #       hanging around forever.  This can happen if the installed Guest Additions
                #       do not support terminating guest processes.
            except:
                fRc = reporter.errorXcpt('Creating stale process #%d failed:' % (i,));
                break;

        try:    cProcesses = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'processes'));
        except: fRc = reporter.errorXcpt();
        else:
            if cProcesses != cStaleProcs:
                fRc = reporter.error('Got %d stale processes, expected %d' % (cProcesses, cStaleProcs));

        if fRc is True:
            #
            # Fire off non-stale processes and wait for termination.
            #
            if oTestVm.isWindows() or oTestVm.isOS2():
                aArgs = [ sCmd, '/C', 'dir', '/S', self.getGuestSystemDir(oTestVm), ];
            else:
                aArgs = [ sCmd, '-c', 'ls -la ' + self.getGuestSystemDir(oTestVm), ];
            reporter.log2('Starting non-stale processes...');
            aoProcesses = [];
            for i in xrange(0, cStaleProcs):
                try:
                    oCurProc = oGuestSession.processCreate(sCmd, aArgs if self.oTstDrv.fpApiVer >= 5.0 else aArgs[1:],
                                                           [], [], 0); # Infinite timeout.
                    aoProcesses.append(oCurProc);
                except:
                    fRc = reporter.errorXcpt('Creating non-stale process #%d failed:' % (i,));
                    break;

            reporter.log2('Waiting for non-stale processes to terminate...');
            for i, oProcess in enumerate(aoProcesses):
                try:
                    oProcess.waitForArray([ vboxcon.ProcessWaitForFlag_Terminate, ], 30 * 1000);
                    eProcessStatus = oProcess.status;
                except:
                    fRc = reporter.errorXcpt('Waiting for non-stale process #%d failed:' % (i,));
                else:
                    if eProcessStatus != vboxcon.ProcessStatus_TerminatedNormally:
                        fRc = reporter.error('Waiting for non-stale processes #%d resulted in status %d, expected %d'
                                             % (i, eProcessStatus, vboxcon.ProcessStatus_TerminatedNormally));

            try:    cProcesses = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'processes'));
            except: fRc = reporter.errorXcpt();
            else:
                # Here we count the stale processes (that is, processes we don't have a reference
                # anymore for) and the started + terminated non-stale processes (that we still keep
                # a reference in aoProcesses[] for).
                if cProcesses != (cStaleProcs * 2):
                    fRc = reporter.error('Got %d total processes, expected %d' % (cProcesses, cStaleProcs));

        if fRc is True:
            reporter.log2('All non-stale processes terminated');

            #
            # Fire off non-stale blocking processes which are terminated via terminate().
            #
            if oTestVm.isWindows() or oTestVm.isOS2():
                aArgs = [ sCmd, '/C', 'pause'];
            else:
                aArgs = [ sCmd ];
            reporter.log2('Starting blocking processes...');
            aoProcesses = [];
            for i in xrange(0, cStaleProcs):
                try:
                    oCurProc = oGuestSession.processCreate(sCmd, aArgs if self.oTstDrv.fpApiVer >= 5.0 else aArgs[1:],
                                                           [],  [], 30 * 1000);
                    # Note: Use a timeout in the call above for not letting the stale processes
                    #       hanging around forever.  This can happen if the installed Guest Additions
                    #       do not support terminating guest processes.
                    aoProcesses.append(oCurProc);
                except:
                    fRc = reporter.errorXcpt('Creating non-stale blocking process #%d failed:' % (i,));
                    break;

            reporter.log2('Terminating blocking processes...');
            for i, oProcess in enumerate(aoProcesses):
                try:
                    oProcess.terminate();
                except: # Termination might not be supported, just skip and log it.
                    reporter.logXcpt('Termination of blocking process #%d failed, skipped:' % (i,));

            # There still should be 20 processes because we terminated the 10 newest ones.
            try:    cProcesses = len(self.oTstDrv.oVBoxMgr.getArray(oGuestSession, 'processes'));
            except: fRc = reporter.errorXcpt();
            else:
                if cProcesses != (cStaleProcs * 2):
                    fRc = reporter.error('Got %d total processes, expected %d' % (cProcesses, cStaleProcs));
                reporter.log2('Final guest session processes count: %d' % (cProcesses,));

        #
        # Now try to close the session and see what happens.
        #
        reporter.log2('Closing guest session ...');
        try:
            oGuestSession.close();
        except:
            fRc = reporter.errorXcpt('Testing for stale processes failed:');

        return (fRc, oTxsSession);

    def testGuestCtrlExec(self, oSession, oTxsSession, oTestVm):                # pylint: disable=too-many-locals,too-many-statements
        """
        Tests the basic execution feature.
        """

        if oTestVm.isWindows() or oTestVm.isOS2():
            sImageOut = self.getGuestSystemShell(oTestVm);
        else:
            sImageOut = "/bin/ls";

        # Use credential defaults.
        oCreds = tdCtxCreds();
        oCreds.applyDefaultsIfNotSet(oTestVm);

        aaInvalid = [
            # Invalid parameters.
            [ tdTestExec(), tdTestResultExec() ],
            # Non-existent / invalid image.
            [ tdTestExec(sCmd = "non-existent"), tdTestResultExec() ],
            [ tdTestExec(sCmd = "non-existent2"), tdTestResultExec() ],
            # Use an invalid format string.
            [ tdTestExec(sCmd = "%$%%%&"), tdTestResultExec() ],
            # More stuff.
            [ tdTestExec(sCmd = u"ƒ‰‹ˆ÷‹¸"), tdTestResultExec() ],
            [ tdTestExec(sCmd = "???://!!!"), tdTestResultExec() ],
            [ tdTestExec(sCmd = "<>!\\"), tdTestResultExec() ],
            # Enable as soon as ERROR_BAD_DEVICE is implemented.
            #[ tdTestExec(sCmd = "CON", tdTestResultExec() ],
        ];

        if oTestVm.isWindows():
            sVBoxControl = "C:\\Program Files\\Oracle\\VirtualBox Guest Additions\\VBoxControl.exe";
            sSystem32 = self.getGuestSystemDir(oTestVm);
            aaExec = [
                # Basic execution.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', sSystem32 ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', sSystem32 + '\\kernel32.dll' ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', sSystem32 + '\\nonexist.dll' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', '/wrongparam' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                # Paths with spaces.
                ## @todo Get path of installed Guest Additions. Later.
                [ tdTestExec(sCmd = sVBoxControl, aArgs = [ sVBoxControl, 'version' ]),
                  tdTestResultExec(fRc = True) ],
                # StdOut.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', sSystem32 ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', 'stdout-non-existing' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                # StdErr.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', sSystem32 ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', 'stderr-non-existing' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                # StdOut + StdErr.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', sSystem32 ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'dir', '/S', 'stdouterr-non-existing' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ]
                # FIXME: Failing tests.
                # Environment variables.
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'TEST_NONEXIST' ],
                #   tdTestResultExec(fRc = True, iExitCode = 1) ]
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'windir' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, sBuf = 'windir=C:\\WINDOWS\r\n') ],
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'TEST_FOO' ],
                #              aEnv = [ 'TEST_FOO=BAR' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, sBuf = 'TEST_FOO=BAR\r\n') ],
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'TEST_FOO' ],
                #              aEnv = [ 'TEST_FOO=BAR', 'TEST_BAZ=BAR' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, sBuf = 'TEST_FOO=BAR\r\n') ]

                ## @todo Create some files (or get files) we know the output size of to validate output length!
                ## @todo Add task which gets killed at some random time while letting the guest output something.
            ];
        elif oTestVm.isLinux():
            sVBoxControl = "/usr/bin/VBoxControl"; # Symlink
            aaExec = [
                # Basic execution.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '-R', '/etc' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/bin/sh' ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '--wrong-parameter' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/non/existent' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ],
                # Paths with spaces.
                ## @todo Get path of installed Guest Additions. Later.
                [ tdTestExec(sCmd = sVBoxControl, aArgs = [ sVBoxControl, 'version' ]),
                  tdTestResultExec(fRc = True) ],
                # StdOut.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/etc' ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, 'stdout-non-existing' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ],
                # StdErr.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/etc' ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, 'stderr-non-existing' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ],
                # StdOut + StdErr.
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/etc' ]),
                  tdTestResultExec(fRc = True) ],
                [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, 'stdouterr-non-existing' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ]
                # FIXME: Failing tests.
                # Environment variables.
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'TEST_NONEXIST' ],
                #   tdTestResultExec(fRc = True, iExitCode = 1) ]
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'windir' ],
                #
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, sBuf = 'windir=C:\\WINDOWS\r\n') ],
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'TEST_FOO' ],
                #              aEnv = [ 'TEST_FOO=BAR' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, sBuf = 'TEST_FOO=BAR\r\n') ],
                # [ tdTestExec(sCmd = sImageOut, aArgs = [ sImageOut, '/C', 'set', 'TEST_FOO' ],
                #              aEnv = [ 'TEST_FOO=BAR', 'TEST_BAZ=BAR' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, sBuf = 'TEST_FOO=BAR\r\n') ]

                ## @todo Create some files (or get files) we know the output size of to validate output length!
                ## @todo Add task which gets killed at some random time while letting the guest output something.
            ];

        # Build up the final test array for the first batch.
        aaTests = [];
        aaTests.extend(aaInvalid);
        if aaExec is not None:
            aaTests.extend(aaExec);
        fRc = True;

        if fRc is False:
            return (fRc, oTxsSession);

        #
        # First batch: One session per guest process.
        #
        reporter.log('One session per guest process ...');
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResultExec
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlExec: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            fRc = self.gctrlExecDoTest(i, oCurTest, oCurRes, oCurGuestSession);
            if fRc is False:
                break;
            fRc = oCurTest.closeSession();
            if fRc is False:
                break;

        reporter.log('Execution of all tests done, checking for stale sessions');

        # No sessions left?
        if fRc is True:
            aSessions = self.oTstDrv.oVBoxMgr.getArray(oSession.o.console.guest, 'sessions');
            cSessions   = len(aSessions);
            if cSessions != 0:
                reporter.error('Found %d stale session(s), expected 0:' % (cSessions,));
                for (i, aSession) in enumerate(aSessions):
                    reporter.log('\tStale session #%d ("%s")' % (aSession.id, aSession.name));
                fRc = False;

        if fRc is False:
            return (fRc, oTxsSession);

        reporter.log('Now using one guest session for all tests ...');

        #
        # Second batch: One session for *all* guest processes.
        #
        oGuest = oSession.o.console.guest;
        try:
            reporter.log('Creating session for all tests ...');
            oCurGuestSession = oGuest.createSession(oCreds.sUser, oCreds.sPassword, oCreds.sDomain,
                                                   'testGuestCtrlExec: One session for all tests');
            try:
                aeWaitFor = [ vboxcon.GuestSessionWaitForFlag_Start ];
                waitResult = oCurGuestSession.waitForArray(aeWaitFor, 30 * 1000);
                if waitResult not in (vboxcon.GuestSessionWaitResult_Start, vboxcon.GuestSessionWaitResult_WaitFlagNotSupported):
                    reporter.error('Session did not start successfully, returned wait result: %d' \
                                   % (waitResult));
                    return (False, oTxsSession);
                reporter.log('Session successfully started');
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.logXcpt('Waiting for guest session to start failed:');
                return (False, oTxsSession);
            # Note: Not waiting for the guest session to start here
            #       is intentional. This must be handled by the process execution
            #       call then.
            for (i, aTest) in enumerate(aaTests):
                oCurTest = aTest[0]; # tdTestExec, use an index, later.
                oCurRes  = aTest[1]; # tdTestResultExec
                oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
                fRc = self.gctrlExecDoTest(i, oCurTest, oCurRes, oCurGuestSession);
                if fRc is False:
                    break;
            try:
                reporter.log2('Closing guest session ...');
                oCurGuestSession.close();
                oCurGuestSession = None;
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.logXcpt('Closing guest session failed:');
                fRc = False;
        except:
            reporter.logXcpt('Could not create one session:');

        reporter.log('Execution of all tests done, checking for stale sessions again');

        # No sessions left?
        if fRc is True:
            cSessions = len(self.oTstDrv.oVBoxMgr.getArray(oSession.o.console.guest, 'sessions'));
            if cSessions != 0:
                reporter.error('Found %d stale session(s), expected 0' % (cSessions,));
                fRc = False;

        return (fRc, oTxsSession);

    def threadForTestGuestCtrlSessionReboot(self, oGuestProcess):
        """
        Thread routine which waits for the stale guest process getting terminated (or some error)
        while the main test routine reboots the guest. It then compares the expected guest process result
        and logs an error if appropriate.
        """
        reporter.log('Waiting for stale process getting killed ...');
        waitResult = oGuestProcess.waitForArray([ vboxcon.ProcessWaitForFlag_Terminate ], 5 * 60 * 1000);

        if  waitResult == vboxcon.ProcessWaitResult_Terminate \
        and oGuestProcess.status == vboxcon.ProcessStatus_Down:
            reporter.log('Stale process was correctly terminated (status: down)');
        else:
            reporter.error('Got wrong stale process result: waitResult is %d, current process status is: %d' \
                            % (waitResult, oGuestProcess.status));

    def testGuestCtrlSessionReboot(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests guest object notifications when a guest gets rebooted / shutdown.
        These notifications gets sent from the guest sessions in order to make API clients
        aware of guest session changes.

        For that to test we create a stale guest process and trigger a reboot on the guest.
        """

        sImage = self.getGuestSystemShell(oTestVm);

        # Use credential defaults.
        oCreds = tdCtxCreds();
        oCreds.applyDefaultsIfNotSet(oTestVm);

        fRc = True;

        try:
            reporter.log('Creating session ...');
            oGuest = oSession.o.console.guest;
            oGuestSession = oGuest.createSession(oCreds.sUser, oCreds.sPassword, oCreds.sDomain, 'testGuestCtrlExecReboot');
            try:
                aeWaitFor = [ vboxcon.GuestSessionWaitForFlag_Start ];
                waitResult = oGuestSession.waitForArray(aeWaitFor, 30 * 1000);
                if waitResult not in (vboxcon.GuestSessionWaitResult_Start, vboxcon.GuestSessionWaitResult_WaitFlagNotSupported):
                    reporter.error('Session did not start successfully, returned wait result: %d' \
                                   % (waitResult));
                    return (False, oTxsSession);
                reporter.log('Session successfully started');
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.logXcpt('Waiting for guest session to start failed:');
                return (False, oTxsSession);

            try:
                aArgs = [ sImage ];
                aEnv = [];
                fFlags = [];
                oGuestProcess = oGuestSession.processCreate(sImage,
                                                            aArgs if self.oTstDrv.fpApiVer >= 5.0 else aArgs[1:], aEnv, fFlags,
                                                            30 * 1000);
                waitResult = oGuestProcess.waitForArray([ vboxcon.ProcessWaitForFlag_Start ], 30 * 1000);
                reporter.log2('Starting process wait result returned: %d, current process status is: %d' \
                              % (waitResult, oGuestProcess.status));
            except:
                reporter.logXcpt('Creating stale process failed:');
                fRc = False;

            if fRc:
                reporter.log('Creating reboot thread ...');
                oThreadReboot = threading.Thread(target = self.threadForTestGuestCtrlSessionReboot,
                                                 args=(oGuestProcess,),
                                                 name=('threadForTestGuestCtrlSessionReboot'));
                oThreadReboot.setDaemon(True);
                oThreadReboot.start();

                reporter.log('Waiting for reboot ....');
                time.sleep(15);
                reporter.log('Rebooting guest and reconnecting TxS');

                (oSession, oTxsSession) = self.oTstDrv.txsRebootAndReconnectViaTcp(oSession, oTxsSession, cMsTimeout = 3 * 60000);

                reporter.log('Waiting for thread to finish ...');
                oThreadReboot.join();
            try:
                reporter.log2('Closing guest session ...');
                oGuestSession.close();
                oGuestSession = None;
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.logXcpt('Closing guest session failed:');
                fRc = False;
        except:
            reporter.logXcpt('Could not create one session:');

        return (fRc, oTxsSession);

    def testGuestCtrlExecErrorLevel(self, oSession, oTxsSession, oTestVm):
        """
        Tests handling of error levels from started guest processes.
        """

        sImage = self.getGuestSystemShell(oTestVm);

        aaTests = [];
        if oTestVm.isWindows():
            sSystem32 = self.getGuestSystemDir(oTestVm);
            aaTests.extend([
                # Simple.
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'wrongcommand' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'exit', '22' ]),
                  tdTestResultExec(fRc = True, iExitCode = 22) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'set', 'ERRORLEVEL=234' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'echo', '%WINDIR%' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'set', 'ERRORLEVEL=0' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', sSystem32 ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', sSystem32 + '\\kernel32.dll' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-file' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-dir\\' ]),
                  tdTestResultExec(fRc = True, iExitCode = 1) ]
                # FIXME: Failing tests.
                # With stdout.
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', sSystem32 ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut ]),
                #   tdTestResultExec(fRc = True, iExitCode = 0) ],
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-file' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut ]),
                #   tdTestResultExec(fRc = True, iExitCode = 1) ],
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-dir\\' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut ]),
                #   tdTestResultExec(fRc = True, iExitCode = 1) ],
                # With stderr.
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', sSystem32],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, iExitCode = 0) ],
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-file' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, iExitCode = 1) ],
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-dir\\' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, iExitCode = 1) ],
                # With stdout/stderr.
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', sSystem32 ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, iExitCode = 0) ],
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-file' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, iExitCode = 1) ],
                # [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '/C', 'dir', 'c:\\nonexisting-dir\\' ],
                #              fFlags = [ vboxcon.ProcessCreateFlag_WaitForStdOut, vboxcon.ProcessCreateFlag_WaitForStdErr ]),
                #   tdTestResultExec(fRc = True, iExitCode = 1) ]
                ## @todo Test stdin!
            ]);
        elif oTestVm.isLinux():
            aaTests.extend([
                # Simple.
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'wrongcommand' ]),
                  tdTestResultExec(fRc = True, iExitCode = 127) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'exit 22' ]),
                  tdTestResultExec(fRc = True, iExitCode = 22) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'echo $PWD' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'export MY_ERRORLEVEL=0' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'ls /etc' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'ls /bin/sh' ]),
                  tdTestResultExec(fRc = True, iExitCode = 0) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'ls /non/existing/file' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ],
                [ tdTestExec(sCmd = sImage, aArgs = [ sImage, '-c', 'ls /non/existing/dir/' ]),
                  tdTestResultExec(fRc = True, iExitCode = 2) ]
            ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlExecErrorLevel: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            fRc = self.gctrlExecDoTest(i, oCurTest, oCurRes, oCurGuestSession);
            oCurTest.closeSession();
            if fRc is False:
                break;

        return (fRc, oTxsSession);

    def testGuestCtrlExecTimeout(self, oSession, oTxsSession, oTestVm):
        """
        Tests handling of timeouts of started guest processes.
        """

        sImage = self.getGuestSystemShell(oTestVm);

        # Use credential defaults.
        oCreds = tdCtxCreds();
        oCreds.applyDefaultsIfNotSet(oTestVm);

        fRc = True;
        try:
            oGuest = oSession.o.console.guest;
            oGuestSession = oGuest.createSession(oCreds.sUser, oCreds.sPassword, oCreds.sDomain, "testGuestCtrlExecTimeout");
            oGuestSession.waitForArray([ vboxcon.GuestSessionWaitForFlag_Start ], 30 * 1000);
            # Create a process which never terminates and should timeout when
            # waiting for termination.
            try:
                curProc = oGuestSession.processCreate(sImage, [sImage,] if self.oTstDrv.fpApiVer >= 5.0 else [], \
                                                      [], [], 30 * 1000);
                reporter.log('Waiting for process 1 being started ...');
                waitRes = curProc.waitForArray([ vboxcon.ProcessWaitForFlag_Start ], 30 * 1000);
                if waitRes != vboxcon.ProcessWaitResult_Start:
                    reporter.error('Waiting for process 1 to start failed, got status %d');
                    fRc = False;
                if fRc:
                    reporter.log('Waiting for process 1 to time out within 1ms ...');
                    waitRes = curProc.waitForArray([ vboxcon.ProcessWaitForFlag_Terminate ], 1);
                    if waitRes != vboxcon.ProcessWaitResult_Timeout:
                        reporter.error('Waiting for process 1 did not time out when it should (1)');
                        fRc = False;
                    else:
                        reporter.log('Waiting for process 1 timed out (1), good');
                if fRc:
                    reporter.log('Waiting for process 1 to time out within 5000ms ...');
                    waitRes = curProc.waitForArray([ vboxcon.ProcessWaitForFlag_Terminate ], 5000);
                    if waitRes != vboxcon.ProcessWaitResult_Timeout:
                        reporter.error('Waiting for process 1 did not time out when it should, got wait result %d' % (waitRes,));
                        fRc = False;
                    else:
                        reporter.log('Waiting for process 1 timed out (5000), good');
                ## @todo Add curProc.terminate() as soon as it's implemented.
            except:
                reporter.errorXcpt('Exception for process 1:');
                fRc = False;
            # Create a lengthly running guest process which will be killed by VBoxService on the
            # guest because it ran out of execution time (5 seconds).
            if fRc:
                try:
                    curProc = oGuestSession.processCreate(sImage, [sImage,] if self.oTstDrv.fpApiVer >= 5.0 else [], \
                                                          [], [], 5 * 1000);
                    reporter.log('Waiting for process 2 being started ...');
                    waitRes = curProc.waitForArray([ vboxcon.ProcessWaitForFlag_Start ], 30 * 1000);
                    if waitRes != vboxcon.ProcessWaitResult_Start:
                        reporter.error('Waiting for process 1 to start failed, got status %d');
                        fRc = False;
                    if fRc:
                        reporter.log('Waiting for process 2 to get killed because it ran out of execution time ...');
                        waitRes = curProc.waitForArray([ vboxcon.ProcessWaitForFlag_Terminate ], 30 * 1000);
                        if waitRes != vboxcon.ProcessWaitResult_Timeout:
                            reporter.error('Waiting for process 2 did not time out when it should, got wait result %d' \
                                           % (waitRes,));
                            fRc = False;
                    if fRc:
                        reporter.log('Waiting for process 2 indicated an error, good');
                        if curProc.status != vboxcon.ProcessStatus_TimedOutKilled:
                            reporter.error('Status of process 2 wrong; excepted %d, got %d' \
                                           % (vboxcon.ProcessStatus_TimedOutKilled, curProc.status));
                            fRc = False;
                        else:
                            reporter.log('Status of process 2 correct (%d)' % (vboxcon.ProcessStatus_TimedOutKilled,));
                    ## @todo Add curProc.terminate() as soon as it's implemented.
                except:
                    reporter.errorXcpt('Exception for process 2:');
                    fRc = False;
            oGuestSession.close();
        except:
            reporter.errorXcpt('Could not handle session:');
            fRc = False;

        return (fRc, oTxsSession);

    def testGuestCtrlDirCreate(self, oSession, oTxsSession, oTestVm):
        """
        Tests creation of guest directories.
        """

        if oTestVm.isWindows():
            sScratch  = "C:\\Temp\\vboxtest\\testGuestCtrlDirCreate\\";
        else:
            sScratch  = "/tmp/testGuestCtrlDirCreate/";

        aaTests = [];
        aaTests.extend([
            # Invalid stuff.
            [ tdTestDirCreate(sDirectory = '' ), tdTestResult() ],
            # More unusual stuff.
            [ tdTestDirCreate(sDirectory = '..\\..\\' ), tdTestResult() ],
            [ tdTestDirCreate(sDirectory = '../../' ), tdTestResult() ],
            [ tdTestDirCreate(sDirectory = 'z:\\' ), tdTestResult() ],
            [ tdTestDirCreate(sDirectory = '\\\\uncrulez\\foo' ), tdTestResult() ],
            # Creating directories.
            [ tdTestDirCreate(sDirectory = sScratch ), tdTestResult() ],
            [ tdTestDirCreate(sDirectory = os.path.join(sScratch, 'foo\\bar\\baz'),
                              fFlags = [ vboxcon.DirectoryCreateFlag_Parents ] ),
              tdTestResult(fRc = True) ],
            [ tdTestDirCreate(sDirectory = os.path.join(sScratch, 'foo\\bar\\baz'),
                              fFlags = [ vboxcon.DirectoryCreateFlag_Parents ] ),
              tdTestResult(fRc = True) ],
            # Long (+ random) stuff.
            [ tdTestDirCreate(sDirectory = os.path.join(sScratch,
                                                        "".join(random.choice(string.ascii_lowercase) for i in xrange(32))) ),
              tdTestResult(fRc = True) ],
            [ tdTestDirCreate(sDirectory = os.path.join(sScratch,
                                                        "".join(random.choice(string.ascii_lowercase) for i in xrange(128))) ),
              tdTestResult(fRc = True) ],
            # Following two should fail on Windows (paths too long). Both should timeout.
            [ tdTestDirCreate(sDirectory = os.path.join(sScratch,
                                                        "".join(random.choice(string.ascii_lowercase) for i in xrange(255))) ),
              tdTestResult(fRc = not oTestVm.isWindows()) ],
            [ tdTestDirCreate(sDirectory = os.path.join(sScratch,
                                                        "".join(random.choice(string.ascii_lowercase) for i in xrange(255))) ),
              tdTestResult(fRc = not oTestVm.isWindows()) ]
        ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sDirectory="%s" ...' % (i, oCurTest.sDirectory));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlDirCreate: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            fRc = self.gctrlCreateDir(oCurTest, oCurRes, oCurGuestSession);
            oCurTest.closeSession();
            if fRc is False:
                reporter.error('Test #%d failed' % (i,));
                fRc = False;
                break;

        return (fRc, oTxsSession);

    def testGuestCtrlDirCreateTemp(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests creation of temporary directories.
        """

        aaTests = [];
        if oTestVm.isWindows():
            aaTests.extend([
                # Invalid stuff.
                [ tdTestDirCreateTemp(sDirectory = ''), tdTestResult() ],
                [ tdTestDirCreateTemp(sDirectory = 'C:\\Windows', fMode = 1234), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = '', sDirectory = 'C:\\Windows', fMode = 1234), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = 'xXx', sDirectory = 'C:\\Windows', fMode = 0o700), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = 'xxx', sDirectory = 'C:\\Windows', fMode = 0o700), tdTestResult() ],
                # More unusual stuff.
                [ tdTestDirCreateTemp(sTemplate = 'foo', sDirectory = 'z:\\'), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = 'foo', sDirectory = '\\\\uncrulez\\foo'), tdTestResult() ],
                # Non-existing stuff.
                [ tdTestDirCreateTemp(sTemplate = 'bar', sDirectory = 'c:\\Apps\\nonexisting\\foo'), tdTestResult() ],
                # FIXME: Failing test. Non Windows path
                # [ tdTestDirCreateTemp(sTemplate = 'bar', sDirectory = '/tmp/non/existing'), tdTestResult() ]
            ]);
        elif oTestVm.isLinux():
            aaTests.extend([
                # Invalid stuff.
                [ tdTestDirCreateTemp(sDirectory = ''), tdTestResult() ],
                [ tdTestDirCreateTemp(sDirectory = '/etc', fMode = 1234) ],
                [ tdTestDirCreateTemp(sTemplate = '', sDirectory = '/etc', fMode = 1234), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = 'xXx', sDirectory = '/etc', fMode = 0o700), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = 'xxx', sDirectory = '/etc', fMode = 0o700), tdTestResult() ],
                # More unusual stuff.
                [ tdTestDirCreateTemp(sTemplate = 'foo', sDirectory = 'z:\\'), tdTestResult() ],
                [ tdTestDirCreateTemp(sTemplate = 'foo', sDirectory = '\\\\uncrulez\\foo'), tdTestResult() ],
                # Non-existing stuff.
                [ tdTestDirCreateTemp(sTemplate = 'bar', sDirectory = '/non/existing'), tdTestResult() ],
            ]);

            # FIXME: Failing tests.
            # aaTests.extend([
                # Non-secure variants.
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'X',
                #                       sDirectory = sScratch),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'X',
                #                       sDirectory = sScratch),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fMode = 0o700),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                     sDirectory = sScratch,
                #                     fMode = 0o700),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fMode = 0o755),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fMode = 0o755),
                #   tdTestResult(fRc = True) ],
                # Secure variants.
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch, fSecure = True),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch, fSecure = True),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch, fSecure = True),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch, fSecure = True),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o700),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o700),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o755),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = 'XXX',
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o755),
                #   tdTestResult(fRc = True) ],
                # Random stuff.
                # [ tdTestDirCreateTemp(
                #                       sTemplate = "XXX-".join(random.choice(string.ascii_lowercase) for i in xrange(32)),
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o755),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = "".join('X' for i in xrange(32)),
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o755),
                #   tdTestResult(fRc = True) ],
                # [ tdTestDirCreateTemp(sTemplate = "".join('X' for i in xrange(128)),
                #                       sDirectory = sScratch,
                #                       fSecure = True, fMode = 0o755),
                #   tdTestResult(fRc = True) ]
            # ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sTemplate="%s", fMode=%#o, path="%s", secure="%s" ...' %
                         (i, oCurTest.sTemplate, oCurTest.fMode, oCurTest.sDirectory, oCurTest.fSecure));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlDirCreateTemp: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            sDirTemp = "";
            try:
                sDirTemp = oCurGuestSession.directoryCreateTemp(oCurTest.sTemplate, oCurTest.fMode,
                                                               oCurTest.sDirectory, oCurTest.fSecure);
            except:
                if oCurRes.fRc is True:
                    reporter.errorXcpt('Creating temp directory "%s" failed:' % (oCurTest.sDirectory,));
                    fRc = False;
                    break;
                else:
                    reporter.logXcpt('Creating temp directory "%s" failed expectedly, skipping:' % (oCurTest.sDirectory,));
            oCurTest.closeSession();
            if sDirTemp  != "":
                reporter.log2('Temporary directory is: %s' % (sDirTemp,));
                if self.oTstDrv.fpApiVer >= 5.0:
                    fExists = oCurGuestSession.directoryExists(sDirTemp, False);
                else:
                    fExists = oCurGuestSession.directoryExists(sDirTemp);
                if fExists is False:
                    reporter.error('Test #%d failed: Temporary directory "%s" does not exists' % (i, sDirTemp));
                    fRc = False;
                    break;
        return (fRc, oTxsSession);

    def testGuestCtrlDirRead(self, oSession, oTxsSession, oTestVm):
        """
        Tests opening and reading (enumerating) guest directories.
        """

        sSystemDir = self.getGuestSystemDir(oTestVm);
        atTests = [
            # Invalid stuff.
            [ tdTestDirRead(sDirectory = ''), tdTestResultDirRead() ],
            [ tdTestDirRead(sDirectory = sSystemDir, fFlags = [ 1234 ]), tdTestResultDirRead() ],
            [ tdTestDirRead(sDirectory = sSystemDir, sFilter = '*.foo'), tdTestResultDirRead() ],
            # Non-existing stuff.
            [ tdTestDirRead(sDirectory = oTestVm.pathJoinEx(sSystemDir, 'really-no-such-subdir')), tdTestResultDirRead() ],
            [ tdTestDirRead(sDirectory = oTestVm.pathJoinEx(sSystemDir, 'non', 'existing')), tdTestResultDirRead() ],
        ];

        if oTestVm.isWindows() or oTestVm.isOS2():
            atTests.extend([
                # More unusual stuff.
                [ tdTestDirRead(sDirectory = 'z:\\'), tdTestResultDirRead() ],
                [ tdTestDirRead(sDirectory = '\\\\uncrulez\\foo'), tdTestResultDirRead() ],
            ]);
        else:
            atTests.extend([
                # More unusual stuff.
                [ tdTestDirRead(sDirectory = 'z:/'), tdTestResultDirRead() ],
                [ tdTestDirRead(sDirectory = '\\\\uncrulez\\foo'), tdTestResultDirRead() ],
            ]);
        # Read the system directory (ASSUMES at least 5 files in it):
        atTests.append([ tdTestDirRead(sDirectory = sSystemDir), tdTestResultDirRead(fRc = True, cFiles = -5, cDirs = None) ]);
        ## @todo trailing slash

        ## @todo this is very very inflexible.
        ## Would be better to create a dir tree using TXS and make sure we get it back exactly as expected.
        if oTestVm.sVmName.startswith('tst-xpsp2'):
            atTests.extend([
                # Reading directories.
                [ tdTestDirRead(sDirectory = '../../Windows/Media'),
                  tdTestResultDirRead(fRc = True, cFiles = 38) ],
                [ tdTestDirRead(sDirectory = 'C:\\Windows\\Help'),
                  tdTestResultDirRead(fRc = True, cDirs = 13, cFiles = 574) ],
                [ tdTestDirRead(sDirectory = 'C:\\Windows\\Web'),
                  tdTestResultDirRead(fRc = True, cDirs = 3, cFiles = 49) ]
            ]);


        fRc = True;
        for (i, tTest) in enumerate(atTests):
            oCurTest = tTest[0]   # type: tdTestExec
            oCurRes  = tTest[1]   # type: tdTestResultDirRead

            reporter.log('Testing #%d, dir="%s" ...' % (i, oCurTest.sDirectory));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlDirRead: Test #%d' % (i,), True);
            if fRc is not True:
                break;
            (fRc2, cDirs, cFiles, cOthers) = self.gctrlReadDirTree(oCurTest, oCurGuestSession, oCurRes.fRc);
            oCurTest.closeSession(True);

            reporter.log2('Test #%d: Returned %d directories, %d files total' % (i, cDirs, cFiles));
            if fRc2 is oCurRes.fRc:
                if fRc2 is True:
                    if oCurRes.cFiles is None:
                        pass; # ignore
                    elif oCurRes.cFiles >= 0 and cFiles != oCurRes.cFiles:
                        fRc = reporter.error('Test #%d failed: Got %d files, expected %d' % (i, cFiles, oCurRes.cFiles));
                    elif oCurRes.cFiles < 0 and cFiles < -oCurRes.cFiles:
                        fRc = reporter.error('Test #%d failed: Got %d files, expected at least %d'
                                             % (i, cFiles, -oCurRes.cFiles));
                    if oCurRes.cDirs is None:
                        pass; # ignore
                    elif oCurRes.cDirs >= 0 and cDirs != oCurRes.cDirs:
                        fRc = reporter.error('Test #%d failed: Got %d directories, expected %d' % (i, cDirs, oCurRes.cDirs));
                    elif oCurRes.cDirs < 0 and cDirs < -oCurRes.cDirs:
                        fRc = reporter.error('Test #%d failed: Got %d directories, expected at least %d'
                                             % (i, cDirs, -oCurRes.cDirs));
                    if oCurRes.cOthers is None:
                        pass; # ignore
                    elif oCurRes.cOthers >= 0 and cOthers != oCurRes.cOthers:
                        fRc = reporter.error('Test #%d failed: Got %d other types, expected %d' % (i, cOthers, oCurRes.cOthers));
                    elif oCurRes.cOthers < 0 and cOthers < -oCurRes.cOthers:
                        fRc = reporter.error('Test #%d failed: Got %d other types, expected at least %d'
                                             % (i, cOthers, -oCurRes.cOthers));

            else:
                fRc = reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc2, oCurRes.fRc));

        return (fRc, oTxsSession);

    def testGuestCtrlFileRemove(self, oSession, oTxsSession, oTestVm):
        """
        Tests removing guest files.
        """

        ## @todo r=bird: This fails on windows 7 RTM.  Just create a stupid file and delete it again,
        #                this chord.wav stuff is utter nonsense.
        if oTestVm.isWindows():
            sFileToDelete = "c:\\Windows\\Media\\chord.wav";
        else:
            sFileToDelete = "/home/vbox/.profile";

        aaTests = [];
        if oTestVm.isWindows():
            aaTests.extend([
                # Invalid stuff.
                [ tdTestFileRemove(sFile = ''), tdTestResult() ],
                [ tdTestFileRemove(sFile = 'C:\\Windows'), tdTestResult() ],
                # More unusual stuff.
                [ tdTestFileRemove(sFile = 'z:\\'), tdTestResult() ],
                [ tdTestFileRemove(sFile = '\\\\uncrulez\\foo'), tdTestResult() ],
                # Non-existing stuff.
                [ tdTestFileRemove(sFile = 'c:\\Apps\\nonexisting'), tdTestResult() ],
                # Try to delete system files.
                [ tdTestFileRemove(sFile = 'c:\\pagefile.sys'), tdTestResult() ],
                [ tdTestFileRemove(sFile = 'c:\\Windows\\kernel32.sys'), tdTestResult() ] ## r=bird: it's in \system32\ ...
            ]);

            if oTestVm.sKind == "WindowsXP":
                aaTests.extend([
                    # Try delete some unimportant media stuff.
                    [ tdTestFileRemove(sFile = 'c:\\Windows\\Media\\chimes.wav'), tdTestResult(fRc = True) ],
                    # Second attempt should fail.
                    [ tdTestFileRemove(sFile = 'c:\\Windows\\Media\\chimes.wav'), tdTestResult() ]
                ]);
        elif oTestVm.isLinux():
            aaTests.extend([
                # Invalid stuff.
                [ tdTestFileRemove(sFile = ''), tdTestResult() ],
                [ tdTestFileRemove(sFile = 'C:\\Windows'), tdTestResult() ],
                # More unusual stuff.
                [ tdTestFileRemove(sFile = 'z:/'), tdTestResult() ],
                [ tdTestFileRemove(sFile = '//uncrulez/foo'), tdTestResult() ],
                # Non-existing stuff.
                [ tdTestFileRemove(sFile = '/non/existing'), tdTestResult() ],
                # Try to delete system files.
                [ tdTestFileRemove(sFile = '/etc'), tdTestResult() ],
                [ tdTestFileRemove(sFile = '/bin/sh'), tdTestResult() ]
            ]);

        aaTests.extend([
            # Try delete some unimportant stuff.
            [ tdTestFileRemove(sFile = sFileToDelete), tdTestResult(fRc = True) ],
            # Second attempt should fail.
            [ tdTestFileRemove(sFile = sFileToDelete), tdTestResult() ]
        ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, file="%s" ...' % (i, oCurTest.sFile));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlFileRemove: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            try:
                if self.oTstDrv.fpApiVer >= 5.0:
                    oCurGuestSession.fsObjRemove(oCurTest.sFile);
                else:
                    oCurGuestSession.fileRemove(oCurTest.sFile);
            except:
                if oCurRes.fRc is True:
                    reporter.errorXcpt('Removing file "%s" failed:' % (oCurTest.sFile,));
                    fRc = False;
                    break;
                else:
                    reporter.logXcpt('Removing file "%s" failed expectedly, skipping:' % (oCurTest.sFile,));
            oCurTest.closeSession();
        return (fRc, oTxsSession);

    def testGuestCtrlFileStat(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests querying file information through stat.
        """

        # Basic stuff, existing stuff.
        aoTests = [
            tdTestSessionEx([ tdStepStatDir('.'),
                              tdStepStatDir('..'),
                            ]),
        ];
        if oTestVm.isWindows():
            aoTests += [ tdTestSessionEx([ tdStepStatDir('C:\\Windows'),
                                           tdStepStatDir('C:\\Windows\\System32'),
                                           tdStepStatDir('C:\\Windows\\System32\\'),
                                           tdStepStatDir('C:\\Windows\\System32\\.'),
                                           tdStepStatDir('C:\\Windows\\System32\\.\\'),
                                           tdStepStatDir('C:\\Windows\\System32\\..'),
                                           tdStepStatDir('C:\\Windows\\System32\\..\\'),
                                           tdStepStatDir('C:\\Windows\\System32\\..\\\\'),
                                           tdStepStatDir('C:\\Windows\\System32\\\\..\\\\'),
                                           tdStepStatDir('C:/Windows/System32'),
                                           tdStepStatDir('C:/Windows/System32/'),
                                           tdStepStatDir('c:/winDowS/sYsTeM32/'),
                                           tdStepStatDir('C:/Windows/System32/.'),
                                           tdStepStatDir('C:/Windows/System32/./'),
                                           tdStepStatDir('C:/Windows/System32/..'),
                                           tdStepStatDir('C:/Windows/System32/../'),
                                           tdStepStatDir('C:/Windows/System32/..//'),
                                           tdStepStatDir('C:/Windows/System32//..//'),
                                           tdStepStatFile('C:\\Windows\\System32\\kernel32.dll'),
                                           tdStepStatFile('C:/Windows/System32/kernel32.dll')
                                         ]) ];
        elif oTestVm.isOS2():
            aoTests += [ tdTestSessionEx([ tdStepStatDir('C:\\OS2'),
                                           tdStepStatDir('C:\\OS2\\DLL'),
                                           tdStepStatDir('C:\\OS2\\DLL\\'),
                                           tdStepStatDir('C:/OS2/DLL'),
                                           tdStepStatDir('c:/OS2/DLL'),
                                           tdStepStatDir('c:/OS2/DLL/'),
                                           tdStepStatFile('C:\\CONFIG.SYS'),
                                           tdStepStatFile('C:\\OS2\\DLL\\DOSCALL1.DLL'),
                                         ]) ];
        else: # generic unix.
            aoTests += [ tdTestSessionEx([ tdStepStatDir('/'),
                                           tdStepStatDir('///'),
                                           tdStepStatDir('/usr/bin/.'),
                                           tdStepStatDir('/usr/bin/./'),
                                           tdStepStatDir('/usr/bin/..'),
                                           tdStepStatDir('/usr/bin/../'),
                                           tdStepStatFile('/bin/ls'),
                                           tdStepStatFile('/bin/cp'),
                                           tdStepStatFile('/bin/date'),
                                         ]) ];
        # None existing stuff.
        if oTestVm.isWindows() or oTestVm.isOS2():
            aoTests += [ tdTestSessionEx([ tdStepStatFileNotFound('C:\\NoSuchFileOrDirectory', ),
                                           tdStepStatPathNotFound('C:\\NoSuchDirectory\\'),
                                           tdStepStatPathNotFound('C:/NoSuchDirectory/'),
                                           tdStepStatPathNotFound('C:\\NoSuchDirectory\\.'),
                                           tdStepStatPathNotFound('C:/NoSuchDirectory/.'),
                                           tdStepStatPathNotFound('C:\\NoSuchDirectory\\NoSuchFileOrDirectory'),
                                           tdStepStatPathNotFound('C:/NoSuchDirectory/NoSuchFileOrDirectory'),
                                           tdStepStatPathNotFound('C:/NoSuchDirectory/NoSuchFileOrDirectory/'),
                                           tdStepStatPathNotFound('N:\\'), # ASSUMES nothing mounted on N:!
                                           tdStepStatPathNotFound('\\\\NoSuchUncServerName\\NoSuchShare'),
                                         ]) ];
        else: # generic unix.
            aoTests += [ tdTestSessionEx([ tdStepStatFileNotFound('/NoSuchFileOrDirectory', ),
                                           tdStepStatFileNotFound('/bin/NoSuchFileOrDirectory'),
                                           tdStepStatPathNotFound('/NoSuchDirectory/'),
                                           tdStepStatPathNotFound('/NoSuchDirectory/.'),
                                         ]) ];
        # Invalid parameter check.
        aoTests += [ tdTestSessionEx([ tdStepStat('', vbox.ComError.E_INVALIDARG), ]), ];

        # Some test VM specific tests.
        if oTestVm.sVmName.startswith('tst-xpsp2'):
            aoTests += [ tdTestSessionEx([ tdStepStatFileSize('c:\\Windows\\system32\\kernel32.dll', 983552), ]) ];

        #
        # Execute the tests.
        #
        return tdTestSessionEx.executeListTestSessions(aoTests, self.oTstDrv, oSession, oTxsSession, oTestVm, 'FsStat');

    def testGuestCtrlFileRead(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests reading from guest files.
        """

        if oTxsSession.syncMkDir('${SCRATCH}/testGuestCtrlFileRead') is False:
            reporter.error('Could not create scratch directory on guest');
            return (False, oTxsSession);

        aaTests = [];
        aaTests.extend([
            # Invalid stuff.
            [ tdTestFileReadWrite(cbToReadWrite = 0), tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = ''), tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = 'non-existing.file'), tdTestResultFileReadWrite() ],
            # Wrong open mode.
            [ tdTestFileReadWrite(sFile = 'non-existing.file', sOpenMode = 'rt', sDisposition = 'oe'),
              tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = '\\\\uncrulez\\non-existing.file', sOpenMode = 'tr', sDisposition = 'oe'),
              tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = '../../non-existing.file', sOpenMode = 'wr', sDisposition = 'oe'),
              tdTestResultFileReadWrite() ],
            # Wrong disposition.
            [ tdTestFileReadWrite(sFile = 'non-existing.file', sOpenMode = 'r', sDisposition = 'e'),
              tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = '\\\\uncrulez\\non-existing.file', sOpenMode = 'r', sDisposition = 'o'),
              tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = '../../non-existing.file', sOpenMode = 'r', sDisposition = 'c'),
              tdTestResultFileReadWrite() ],
            # Opening non-existing file when it should exist.
            [ tdTestFileReadWrite(sFile = 'non-existing.file', sOpenMode = 'r', sDisposition = 'oe'),
              tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = '\\\\uncrulez\\non-existing.file', sOpenMode = 'r', sDisposition = 'oe'),
              tdTestResultFileReadWrite() ],
            [ tdTestFileReadWrite(sFile = '../../non-existing.file', sOpenMode = 'r', sDisposition = 'oe'),
              tdTestResultFileReadWrite() ]
        ]);

        if oTestVm.isWindows():
            aaTests.extend([
                # Create a file which must not exist (but it hopefully does).
                [ tdTestFileReadWrite(sFile = 'C:\\Windows\\System32\\calc.exe', sOpenMode = 'w', sDisposition = 'ce'),
                  tdTestResultFileReadWrite() ],
                # Open a file which must exist.
                [ tdTestFileReadWrite(sFile = 'C:\\Windows\\System32\\kernel32.dll', sOpenMode = 'r', sDisposition = 'oe'),
                  tdTestResultFileReadWrite(fRc = True) ],
                # Try truncating a file which already is opened with a different sharing mode (and thus should fail).
                [ tdTestFileReadWrite(sFile = 'C:\\Windows\\System32\\kernel32.dll', sOpenMode = 'w', sDisposition = 'ot'),
                  tdTestResultFileReadWrite() ]
            ]);

            # Note: tst-xppro has other contents in eula.txt.
            if oTestVm.sVmName.startswith('tst-xpsp2'):
                aaTests.extend([
                    # Reading from beginning.
                    [ tdTestFileReadWrite(sFile = 'C:\\Windows\\System32\\eula.txt',
                                        sOpenMode = 'r', sDisposition = 'oe', cbToReadWrite = 33),
                      tdTestResultFileReadWrite(fRc = True, abBuf = 'Microsoft(r) Windows(r) XP Profes',
                                                cbProcessed = 33, offFile = 33) ],
                    # Reading from offset.
                    [ tdTestFileReadWrite(sFile = 'C:\\Windows\\System32\\eula.txt',
                                        sOpenMode = 'r', sDisposition = 'oe', offFile = 17769, cbToReadWrite = 31),
                      tdTestResultFileReadWrite(fRc = True, abBuf = 'only with the HARDWARE. If\x0d\x0a   ',
                                                cbProcessed = 31, offFile = 17769 + 31) ]
                ]);
        elif oTestVm.isLinux():
            aaTests.extend([
                # Create a file which must not exist (but it hopefully does).
                [ tdTestFileReadWrite(sFile = '/etc/issue', sOpenMode = 'w', sDisposition = 'ce'),
                  tdTestResultFileReadWrite() ],
                # Open a file which must exist.
                [ tdTestFileReadWrite(sFile = '/etc/issue', sOpenMode = 'r', sDisposition = 'oe'),
                  tdTestResultFileReadWrite(fRc = True) ],
                # Try truncating a file which already is opened with a different sharing mode (and thus should fail).
                [ tdTestFileReadWrite(sFile = '/etc/issue', sOpenMode = 'w', sDisposition = 'ot'),
                  tdTestResultFileReadWrite() ]
            ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestFileReadWrite, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sFile="%s", cbToReadWrite=%d, sOpenMode="%s", sDisposition="%s", offFile=%d ...'
                         % (i, oCurTest.sFile, oCurTest.cbToReadWrite, oCurTest.sOpenMode,
                            oCurTest.sDisposition, oCurTest.offFile));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlFileRead: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            try:
                fRc2 = True;
                if oCurTest.offFile > 0: # The offset parameter is gone.
                    if self.oTstDrv.fpApiVer >= 5.0:
                        curFile = oCurGuestSession.fileOpenEx(oCurTest.sFile, oCurTest.getAccessMode(), oCurTest.getOpenAction(),
                                                              oCurTest.getSharingMode(), oCurTest.fCreationMode, []);
                        curFile.seek(oCurTest.offFile, vboxcon.FileSeekOrigin_Begin);
                    else:
                        curFile = oCurGuestSession.fileOpenEx(oCurTest.sFile, oCurTest.sOpenMode, oCurTest.sDisposition,
                                                              oCurTest.sSharingMode, oCurTest.fCreationMode, oCurTest.offFile);
                    curOffset = long(curFile.offset);
                    resOffset = long(oCurTest.offFile);
                    if curOffset != resOffset:
                        reporter.error('Test #%d failed: Initial offset on open does not match: Got %d, expected %d'
                                       % (i, curOffset, resOffset));
                        fRc2 = False;
                else:
                    if self.oTstDrv.fpApiVer >= 5.0:
                        curFile = oCurGuestSession.fileOpen(oCurTest.sFile, oCurTest.getAccessMode(), oCurTest.getOpenAction(),
                                                            oCurTest.fCreationMode);
                    else:
                        curFile = oCurGuestSession.fileOpen(oCurTest.sFile, oCurTest.sOpenMode, oCurTest.sDisposition,
                                                            oCurTest.fCreationMode);
                if fRc2 and oCurTest.cbToReadWrite > 0:
                    ## @todo Split this up in 64K reads. Later.
                    ## @todo Test timeouts.
                    aBufRead = curFile.read(oCurTest.cbToReadWrite, 30 * 1000);
                    if  oCurRes.cbProcessed > 0 \
                    and oCurRes.cbProcessed != len(aBufRead):
                        reporter.error('Test #%d failed: Read buffer length does not match: Got %d, expected %d'
                                       % (i, len(aBufRead), oCurRes.cbProcessed));
                        fRc2 = False;
                    if fRc2:
                        if  oCurRes.abBuf is not None \
                        and not utils.areBytesEqual(oCurRes.abBuf, aBufRead):
                            reporter.error('Test #%d failed: Got buffer:\n"%s" (%d bytes, type %s)\n'
                                           'Expected buffer:\n"%s" (%d bytes, type %s)'
                                           % (i, map(hex, map(ord, aBufRead)), len(aBufRead), type(aBufRead),
                                              map(hex, map(ord, oCurRes.abBuf)), len(oCurRes.abBuf), type(oCurRes.abBuf),));
                            reporter.error('Test #%d failed: Got buffer:\n"%s"\nExpected buffer:\n"%s"'
                                           % (i, aBufRead, oCurRes.abBuf));
                            fRc2 = False;
                # Test final offset.
                curOffset = long(curFile.offset);
                resOffset = long(oCurRes.offFile);
                if curOffset != resOffset:
                    reporter.error('Test #%d failed: Final offset does not match: Got %d, expected %d' \
                                   % (i, curOffset, resOffset));
                    fRc2 = False;
                curFile.close();

                if fRc2 != oCurRes.fRc:
                    reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc2, oCurRes.fRc));
                    fRc = False;

            except:
                reporter.logXcpt('Opening "%s" failed:' % (oCurTest.sFile,));
                fRc = False;

            oCurTest.closeSession();

        return (fRc, oTxsSession);

    def testGuestCtrlFileWrite(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests writing to guest files.
        """

        if oTestVm.isWindows():
            sScratch = "C:\\Temp\\vboxtest\\testGuestCtrlFileWrite\\";
        else:
            sScratch = "/tmp/";

        if oTxsSession.syncMkDir('${SCRATCH}/testGuestCtrlFileWrite') is False:
            reporter.error('Could not create scratch directory on guest');
            return (False, oTxsSession);

        aaTests = [];

        cbScratchBuf = random.randint(1, 4096);
        abScratchBuf = os.urandom(cbScratchBuf);
        aaTests.extend([
            # Write to a non-existing file.
            [ tdTestFileReadWrite(sFile = sScratch + 'testGuestCtrlFileWrite.txt',
                                  sOpenMode = 'w+', sDisposition = 'ce', cbToReadWrite = cbScratchBuf, abBuf = abScratchBuf),
              tdTestResultFileReadWrite(fRc = True, abBuf = abScratchBuf, cbProcessed = cbScratchBuf, offFile = cbScratchBuf) ],
        ]);

        aScratchBuf2 = os.urandom(cbScratchBuf);
        aaTests.extend([
            # Append the same amount of data to the just created file.
            [ tdTestFileReadWrite(sFile = sScratch + 'testGuestCtrlFileWrite.txt',
                                  sOpenMode = 'w+', sDisposition = 'oa', cbToReadWrite = cbScratchBuf,
                                  offFile = cbScratchBuf, abBuf = aScratchBuf2),
              tdTestResultFileReadWrite(fRc = True, abBuf = aScratchBuf2, cbProcessed = cbScratchBuf,
                                        offFile = cbScratchBuf * 2) ],
        ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestFileReadWrite, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sFile="%s", cbToReadWrite=%d, sOpenMode="%s", sDisposition="%s", offFile=%d ...'
                         %  (i, oCurTest.sFile, oCurTest.cbToReadWrite, oCurTest.sOpenMode,
                             oCurTest.sDisposition, oCurTest.offFile,));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlFileWrite: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;

            try:
                if oCurTest.offFile > 0: # The offset parameter is gone.
                    if self.oTstDrv.fpApiVer >= 5.0:
                        curFile = oCurGuestSession.fileOpenEx(oCurTest.sFile, oCurTest.getAccessMode(), oCurTest.getOpenAction(),
                                                             oCurTest.getSharingMode(), oCurTest.fCreationMode, []);
                        curFile.seek(oCurTest.offFile, vboxcon.FileSeekOrigin_Begin);
                    else:
                        curFile = oCurGuestSession.fileOpenEx(oCurTest.sFile, oCurTest.sOpenMode, oCurTest.sDisposition,
                                                             oCurTest.sSharingMode, oCurTest.fCreationMode, oCurTest.offFile);
                    curOffset = long(curFile.offset);
                    resOffset = long(oCurTest.offFile);
                    if curOffset != resOffset:
                        reporter.error('Test #%d failed: Initial offset on open does not match: Got %d, expected %d' \
                                       % (i, curOffset, resOffset));
                        fRc = False;
                else:
                    if self.oTstDrv.fpApiVer >= 5.0:
                        curFile = oCurGuestSession.fileOpenEx(oCurTest.sFile, oCurTest.getAccessMode(), oCurTest.getOpenAction(),
                                                             oCurTest.getSharingMode(), oCurTest.fCreationMode, []);
                    else:
                        curFile = oCurGuestSession.fileOpen(oCurTest.sFile, oCurTest.sOpenMode, oCurTest.sDisposition,
                                                           oCurTest.fCreationMode);
                if fRc and oCurTest.cbToReadWrite > 0:
                    reporter.log("File '%s' opened" % oCurTest.sFile);
                    ## @todo Split this up in 64K writes. Later.
                    ## @todo Test timeouts.
                    cBytesWritten = curFile.write(array('b', oCurTest.abBuf), 30 * 1000);
                    if    oCurRes.cbProcessed > 0 \
                      and oCurRes.cbProcessed != cBytesWritten:
                        reporter.error('Test #%d failed: Written buffer length does not match: Got %d, expected %d' \
                                       % (i, cBytesWritten, oCurRes.cbProcessed));
                        fRc = False;
                    if fRc:
                        # Verify written content by seeking back to the initial offset and
                        # re-read & compare the written data.
                        try:
                            if self.oTstDrv.fpApiVer >= 5.0:
                                curFile.seek(-(oCurTest.cbToReadWrite), vboxcon.FileSeekOrigin_Current);
                            else:
                                curFile.seek(-(oCurTest.cbToReadWrite), vboxcon.FileSeekType_Current);
                        except:
                            reporter.logXcpt('Seeking back to initial write position failed:');
                            fRc = False;
                        if fRc and long(curFile.offset) != oCurTest.offFile:
                            reporter.error('Test #%d failed: Initial write position does not match current position, ' \
                                           'got %d, expected %d' % (i, long(curFile.offset), oCurTest.offFile));
                            fRc = False;
                    if fRc:
                        aBufRead = curFile.read(oCurTest.cbToReadWrite, 30 * 1000);
                        if len(aBufRead) != oCurTest.cbToReadWrite:
                            reporter.error('Test #%d failed: Got buffer length %d, expected %d' \
                                           % (i, len(aBufRead), oCurTest.cbToReadWrite));
                            fRc = False;
                        if     fRc \
                           and oCurRes.abBuf is not None \
                           and bytes(oCurRes.abBuf) != bytes(aBufRead):
                            reporter.error('Test #%d failed: Read back buffer (%d bytes) does not match ' \
                                           'written content (%d bytes)' % (i, len(aBufRead), len(aBufRead)));

                            curFile.close();

                            # Download written file from guest.
                            aGstFiles = [];
                            aGstFiles.append(oCurTest.sFile.replace('\\', '/'));
                            self.oTstDrv.txsDownloadFiles(oSession, oTxsSession, aGstFiles, fIgnoreErrors = True);

                            # Create files with buffer contents and upload those for later (manual) inspection.
                            oCurTest.uploadLogData(self.oTstDrv, oCurRes.abBuf, ('testGuestCtrlWriteTest%d-BufExcepted' % i),
                                                                             ('Test #%d: Expected buffer' % i));
                            oCurTest.uploadLogData(self.oTstDrv, aBufRead,    ('testGuestCtrlWriteTest%d-BufGot' % i),
                                                                             ('Test #%d: Got buffer' % i));
                            fRc = False;
                # Test final offset.
                curOffset = long(curFile.offset);
                resOffset = long(oCurRes.offFile);
                if curOffset != resOffset:
                    reporter.error('Test #%d failed: Final offset does not match: Got %d, expected %d' \
                                   % (i, curOffset, resOffset));
                    fRc = False;
                if curFile.status == vboxcon.FileStatus_Open:
                    curFile.close();
                reporter.log("File '%s' closed" % oCurTest.sFile);
            except:
                reporter.logXcpt('Opening "%s" failed:' % (oCurTest.sFile,));
                fRc = False;

            oCurTest.closeSession();

            if fRc != oCurRes.fRc:
                reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc, oCurRes.fRc));
                fRc = False;
                break;

        return (fRc, oTxsSession);

    def testGuestCtrlCopyTo(self, oSession, oTxsSession, oTestVm):
        """
        Tests copying files from host to the guest.
        """

        if oTestVm.isWindows():
            sScratchGst = "C:\\Temp\\vboxtest\\testGuestCtrlCopyTo\\";
            sScratchGstNotExist = "C:\\does-not-exist\\";
            sScratchGstInvalid = "?*|invalid-name?*|";
        else:
            sScratchGst = "/tmp/"; ## @todo Use "${SCRATCH}/testGuestCtrlCopyTo" as soon as TXS CHMOD is implemented.
            sScratchGstNotExist = "/tmp/does-not-exist/";
            sScratchGstInvalid = "/";

        if oTxsSession.syncMkDir('${SCRATCH}/testGuestCtrlCopyTo') is False:
            reporter.error('Could not create scratch directory on guest');
            return (False, oTxsSession);

        ## @todo r=klaus It's not good to use files with unpredictable size
        # for testing. Causes all sorts of weird failures as things grow,
        # exceeding the free space of the test VMs. Especially as this used
        # the very big (and quickly growing) validation kit ISO originally.
        sTestFileBig = self.oTstDrv.getFullResourceName('5.3/guestctrl/50mb_rnd.dat');
        if not os.path.isfile(sTestFileBig):
            sTestFileBig = self.oTstDrv.getGuestAdditionsIso();
        if sTestFileBig == '' or not os.path.isfile(sTestFileBig):
            sTestFileBig = self.oTstDrv.sVBoxValidationKitIso;
        if os.path.isfile(sTestFileBig):
            reporter.log('Test file for big copy found at: %s' % (sTestFileBig,));
        else:
            reporter.log('Warning: Test file for big copy not found -- some tests might fail');

        aaTests = [];
        aaTests.extend([
            # Destination missing.
            [ tdTestCopyTo(sSrc = ''), tdTestResult() ],
            [ tdTestCopyTo(sSrc = '/placeholder', fFlags = [ 80 ] ), tdTestResult() ],
            # Source missing.
            [ tdTestCopyTo(sDst = ''), tdTestResult() ],
            [ tdTestCopyTo(sDst = '/placeholder', fFlags = [ 80 ] ), tdTestResult() ],
            # Testing DirectoryCopyFlag flags.
            [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGstInvalid, fFlags = [ 80 ] ), tdTestResult() ],
            # Testing FileCopyFlag flags.
            [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGstInvalid, fFlags = [ 80 ] ), tdTestResult() ],
            # Nothing to copy (source and/or destination is empty).
            [ tdTestCopyTo(sSrc = 'z:\\'), tdTestResult() ],
            [ tdTestCopyTo(sSrc = '\\\\uncrulez\\foo'), tdTestResult() ],
            [ tdTestCopyTo(sSrc = 'non-exist', sDst = os.path.join(sScratchGst, 'non-exist.dll')), tdTestResult() ]
        ]);

        #
        # Single file handling.
        #
        if self.oTstDrv.fpApiVer > 5.2:
            aaTests.extend([
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGstInvalid), tdTestResult() ],
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGstNotExist), tdTestResult() ],
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGstNotExist), tdTestResult() ],
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = os.path.join(sScratchGstNotExist, 'renamedfile.dll')),
                  tdTestResult() ],
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = os.path.join(sScratchGst, 'HostGABig.dat')),
                  tdTestResult(fRc = True) ],
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = os.path.join(sScratchGst, 'HostGABig.dat')),
                  tdTestResult(fRc = True) ],
                # Note: Copying files into directories via Main is supported only in versions > 5.2.
                # Destination is a directory.
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGst),
                  tdTestResult(fRc = True) ],
                # Copy over file again into same directory (overwrite).
                [ tdTestCopyTo(sSrc = sTestFileBig, sDst = sScratchGst),
                  tdTestResult(fRc = True) ]
            ]);

            if oTestVm.isWindows():
                aaTests.extend([
                    # Copy the same file over to the guest, but this time store the file into the former
                    # file's ADS (Alternate Data Stream). Only works on Windows, of course.
                    [ tdTestCopyTo(sSrc = sTestFileBig, sDst = os.path.join(sScratchGst, 'HostGABig.dat:ADS-Test')),
                      tdTestResult(fRc = True) ]
                ]);

            #
            # Directory handling.
            #
            ## @todo r=michaln disabled completely, can fill up the guest disk or fail without giving a reason
            if self.oTstDrv.fpApiVer > 6.0: # Copying directories via Main is supported only in versions > 5.2.
                if self.oTstDrv.sHost == "win":
                    sSystemRoot = os.getenv('SystemRoot', 'C:\\Windows')
                    aaTests.extend([
                        # Copying directories with contain files we don't have read access to.
                        ## @todo r=klaus disabled, because this can fill up the guest disk, making other tests fail,
                        ## additionally it's not really clear if this fails reliably on all Windows versions, even
                        ## the old ones like XP with a "proper" administrator.
                        #[ tdTestCopyTo(sSrc = os.path.join(sSystemRoot, 'security'),
                        #               sDst = sScratchGst, fFlags = [ vboxcon.DirectoryCopyFlag_CopyIntoExisting ]),
                        #
                        # Copying directories with regular files.
                        [ tdTestCopyTo(sSrc = os.path.join(sSystemRoot, 'Help'),
                                       sDst = sScratchGst, fFlags = [ vboxcon.DirectoryCopyFlag_CopyIntoExisting ]),
                          tdTestResult(fRc = True) ]
                        ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sSrc=%s, sDst=%s, fFlags=%s ...' % \
                         (i, oCurTest.sSrc, oCurTest.sDst, oCurTest.fFlags));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, oCurGuestSession = oCurTest.createSession('testGuestCtrlCopyTo: Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;

            fRc2 = False;
            if os.path.isdir(oCurTest.sSrc):
                try:
                    oCurProgress = oCurGuestSession.directoryCopyToGuest(oCurTest.sSrc, oCurTest.sDst, oCurTest.fFlags);
                    if oCurProgress is not None:
                        oProgress = vboxwrappers.ProgressWrapper(oCurProgress, self.oTstDrv.oVBoxMgr, self.oTstDrv, \
                                                                 "gctrlDirCopyTo");
                        try:
                            oProgress.wait();
                            if oProgress.isSuccess():
                                fRc2 = True;
                            else:
                                oProgress.logResult(fIgnoreErrors = True);
                        except:
                            reporter.logXcpt('Waiting exception for sSrc="%s", sDst="%s":' % (oCurTest.sSrc, oCurTest.sDst));
                    else:
                        reporter.error('No progress object returned');
                except:
                    reporter.logXcpt('directoryCopyToGuest exception for sSrc="%s", sDst="%s":' % (oCurTest.sSrc, oCurTest.sDst));
            else:
                fRc2 = self.gctrlCopyFileTo(oCurGuestSession, oCurTest.sSrc, oCurTest.sDst, oCurTest.fFlags);

            oCurTest.closeSession();

            if fRc2 is oCurRes.fRc:
                ## @todo Verify the copied results (size, checksum?).
                pass;
            else:
                reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc2, oCurRes.fRc));
                fRc = False;
                break;

        return (fRc, oTxsSession);

    def testGuestCtrlCopyFrom(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests copying files from guest to the host.
        """

        if oTestVm.isWindows():
            sPathSep         = "\\";
            sSrcDirExisting  = "C:\\Windows\\Web";
            sSrcFileExisting = "C:\\Windows\\system32\\ole32.dll";
        else:
            sPathSep         = "/";
            sSrcDirExisting  = "/bin";
            sSrcFileExisting = "/etc/issue";

        sScratchHst = os.path.join(self.oTstDrv.sScratchPath, "testGctrlCopyFrom");

        if self.oTstDrv.sHost == "win":
            sScratchHstNotExist      = sScratchHst + "\\does-not-exist\\";
            sScratchHstNotExistChain = sScratchHst + "\\does\\not\\exist\\";
            sScratchHstInvalid       = "?*|invalid-name?*|";
        else:
            sScratchHstNotExist      = sScratchHst + "/does-not-exist/";
            sScratchHstNotExistChain = sScratchHst + "/does/not/exist/";
            sScratchHstInvalid       = "/";

        try:
            os.makedirs(sScratchHst);
        except OSError as e:
            if e.errno != errno.EEXIST:
                reporter.error('Failed: Unable to create scratch directory \"%s\"' % (sScratchHst,));
                return (False, oTxsSession);
        reporter.log('Scratch path is: %s' % (sScratchHst,));

        aaTests = [];
        aaTests.extend([
            # Destination missing.
            [ tdTestCopyFrom(sSrc = ''), tdTestResult() ],
            [ tdTestCopyFrom(sSrc = 'Something', fFlags = [ 80 ] ), tdTestResult() ],
            # Source missing.
            [ tdTestCopyFrom(sDst = ''), tdTestResult() ],
            [ tdTestCopyFrom(sDst = 'Something', fFlags = [ 80 ] ), tdTestResult() ],
            # Testing DirectoryCopyFlag flags.
            [ tdTestCopyFrom(sSrc = sSrcDirExisting, sDst = sScratchHstInvalid, fFlags = [ 80 ] ), tdTestResult() ],
            # Testing FileCopyFlag flags.
            [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = sScratchHstInvalid, fFlags = [ 80 ] ), tdTestResult() ],
            # Nothing to copy (sDst is empty / unreachable).
            [ tdTestCopyFrom(sSrc = 'z:\\'), tdTestResult() ],
            [ tdTestCopyFrom(sSrc = '\\\\uncrulez\\foo'), tdTestResult() ],
            [ tdTestCopyFrom(sSrc = 'non-exist', sDst = os.path.join(sScratchHst, 'non-exist')), tdTestResult() ]
        ]);

        #
        # Single file handling.
        #
        if self.oTstDrv.fpApiVer > 5.2:
            reporter.log(("Single file handling"));
            aaTests.extend([
                # Copying single files.
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = sScratchHstInvalid), tdTestResult() ],
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = os.path.join(sScratchHstInvalid, 'tstCopyFrom-renamedfile')),
                  tdTestResult() ],
                # Copy over file using a different destination name.
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = os.path.join(sScratchHst, 'tstCopyFrom-renamedfile')),
                  tdTestResult(fRc = True) ],
                # Copy over same file (and overwrite existing one).
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = os.path.join(sScratchHst, 'tstCopyFrom-renamedfile')),
                  tdTestResult(fRc = True) ],
                # Note: Copying files into directories via Main is supported only in versions > 5.2.
                # Destination is a directory with a trailing slash (should work).
                # See "cp" syntax.
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = sScratchHst + "/"),
                  tdTestResult(fRc = True) ],
                # Destination is a directory (without a trailing slash, should also work).
                # See "cp" syntax.
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = sScratchHst),
                  tdTestResult(fRc = True) ],
                # Destination is a non-existing directory.
                [ tdTestCopyFrom(sSrc = sSrcFileExisting, sDst = sScratchHstNotExist), tdTestResult() ]
            ]);

            #
            # Directory handling.
            #
            if self.oTstDrv.fpApiVer > 5.2: # Copying directories via Main is supported only in versions > 5.2.
                reporter.log(("Directory handling"));
                aaTests.extend([
                    # Copying entire directories (destination is "<sScratchHst>\Web").
                    [ tdTestCopyFrom(sSrc = sSrcDirExisting, sDst = sScratchHst),
                      tdTestResult(fRc = True) ],
                    # Repeat -- this time it should fail, as the destination directory already exists (and
                    # DirectoryCopyFlag_CopyIntoExisting is not specified).
                    [ tdTestCopyFrom(sSrc = sSrcDirExisting, sDst = sScratchHst), tdTestResult() ],
                    # Next try with the DirectoryCopyFlag_CopyIntoExisting flag being set.
                    [ tdTestCopyFrom(sSrc = sSrcDirExisting, sDst = sScratchHst,
                                     fFlags = [ vboxcon.DirectoryCopyFlag_CopyIntoExisting ] ),
                      tdTestResult(fRc = True) ],
                    # Ditto, with trailing slash.
                    [ tdTestCopyFrom(sSrc = sSrcDirExisting,
                                     sDst = sScratchHst + "/", fFlags = [ vboxcon.DirectoryCopyFlag_CopyIntoExisting ]),
                      tdTestResult(fRc = True) ],
                    # Copying contents of directories into a non-existing directory chain on the host which fail.
                    [ tdTestCopyFrom(sSrc = sSrcDirExisting + sPathSep, sDst = sScratchHstNotExistChain,
                                     fFlags = [ vboxcon.DirectoryCopyFlag_CopyIntoExisting ]), tdTestResult() ],
                    # Copying contents of directories into a non-existing directory on the host, which should succeed.
                    [ tdTestCopyFrom(sSrc = sSrcDirExisting + sPathSep, sDst = sScratchHstNotExist,
                                     fFlags = [ vboxcon.DirectoryCopyFlag_CopyIntoExisting ] ),
                      tdTestResult(fRc = True) ]
                ]);

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sSrc="%s", sDst="%s", fFlags="%s" ...'
                         % (i, oCurTest.sSrc, oCurTest.sDst, oCurTest.fFlags,));

            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc2, oCurGuestSession = oCurTest.createSession('testGuestCtrlCopyFrom: Test #%d' % (i,));
            if fRc2 is not True:
                fRc = reporter.error('Test #%d failed: Could not create session' % (i,));
                break;

            try:
                if self.oTstDrv.fpApiVer >= 5.0:
                    oFsInfo = oCurGuestSession.fsObjQueryInfo(oCurTest.sSrc, True); # fFollowSymlinks
                else:
                    oFsInfo = oCurGuestSession.fileQueryInfo(oCurTest.sSrc);
                eFsObjType = oFsInfo.type;
            except:
                if oCurRes.fRc:
                    reporter.errorXcpt('Query information exception for sSrc="%s", sDst="%s":' % (oCurTest.sSrc, oCurTest.sDst,));
                else:
                    reporter.logXcpt(  'Query information exception for sSrc="%s", sDst="%s":' % (oCurTest.sSrc, oCurTest.sDst,));
                fRc2 = False;
            else:
                if eFsObjType == vboxcon.FsObjType_Directory:
                    fRc2 = self.gctrlCopyDirFrom(oCurGuestSession, oCurTest.sSrc, oCurTest.sDst, oCurTest.fFlags, oCurRes.fRc);
                elif eFsObjType == vboxcon.FsObjType_File:
                    fRc2 = self.gctrlCopyFileFrom(oCurGuestSession, oCurTest.sSrc, oCurTest.sDst, oCurTest.fFlags, oCurRes.fRc);
                else:
                    reporter.log2('Element "%s" not handled (yet), skipping' % oFsInfo.name);

            oCurTest.closeSession();
            if fRc2 == oCurRes.fRc:
                pass; ## @todo Verify the copied results (size, checksum?).
            else:
                fRc = reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc2, oCurRes.fRc));

        return (fRc, oTxsSession);

    def testGuestCtrlUpdateAdditions(self, oSession, oTxsSession, oTestVm): # pylint: disable=too-many-locals
        """
        Tests updating the Guest Additions inside the guest.
        """

        # Some stupid trickery to guess the location of the iso.
        sVBoxValidationKitISO = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../VBoxValidationKit.iso'));
        if not os.path.isfile(sVBoxValidationKitISO):
            sVBoxValidationKitISO = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../VBoxTestSuite.iso'));
        if not os.path.isfile(sVBoxValidationKitISO):
            sCur = os.getcwd();
            for i in xrange(0, 10):
                sVBoxValidationKitISO = os.path.join(sCur, 'validationkit/VBoxValidationKit.iso');
                if os.path.isfile(sVBoxValidationKitISO):
                    break;
                sVBoxValidationKitISO = os.path.join(sCur, 'testsuite/VBoxTestSuite.iso');
                if os.path.isfile(sVBoxValidationKitISO):
                    break;
                sCur = os.path.abspath(os.path.join(sCur, '..'));
                if i is None: pass; # shut up pychecker/pylint.
        if os.path.isfile(sVBoxValidationKitISO):
            reporter.log('Validation Kit .ISO found at: %s' % (sVBoxValidationKitISO,));
        else:
            reporter.log('Warning: Validation Kit .ISO not found -- some tests might fail');

        sScratch = os.path.join(self.oTstDrv.sScratchPath, "testGctrlUpdateAdditions");
        try:
            os.makedirs(sScratch);
        except OSError as e:
            if e.errno != errno.EEXIST:
                reporter.error('Failed: Unable to create scratch directory \"%s\"' % (sScratch,));
                return (False, oTxsSession);
        reporter.log('Scratch path is: %s' % (sScratch,));

        aaTests = [];
        if oTestVm.isWindows():
            aaTests.extend([
                # Source is missing.
                [ tdTestUpdateAdditions(sSrc = ''), tdTestResult() ],

                # Wrong fFlags.
                [ tdTestUpdateAdditions(sSrc = self.oTstDrv.getGuestAdditionsIso(),
                                        fFlags = [ 1234 ]), tdTestResult() ],

                # Non-existing .ISO.
                [ tdTestUpdateAdditions(sSrc = "non-existing.iso"), tdTestResult() ],

                # Wrong .ISO.
                [ tdTestUpdateAdditions(sSrc = sVBoxValidationKitISO), tdTestResult() ],

                # The real thing.
                [ tdTestUpdateAdditions(sSrc = self.oTstDrv.getGuestAdditionsIso()),
                  tdTestResult(fRc = True) ],
                # Test the (optional) installer arguments. This will extract the
                # installer into our guest's scratch directory.
                [ tdTestUpdateAdditions(sSrc = self.oTstDrv.getGuestAdditionsIso(),
                                        aArgs = [ '/extract', '/D=' + sScratch ]),
                  tdTestResult(fRc = True) ]
                # Some debg ISO. Only enable locally.
                #[ tdTestUpdateAdditions(
                #                      sSrc = "V:\\Downloads\\VBoxGuestAdditions-r80354.iso"),
                #  tdTestResult(fRc = True) ]
            ]);
        else:
            reporter.log('No OS-specific tests for non-Windows yet!');

        fRc = True;
        for (i, aTest) in enumerate(aaTests):
            oCurTest = aTest[0]; # tdTestExec, use an index, later.
            oCurRes  = aTest[1]; # tdTestResult
            reporter.log('Testing #%d, sSrc="%s", fFlags="%s" ...' % \
                         (i, oCurTest.sSrc, oCurTest.fFlags));
            oCurTest.setEnvironment(oSession, oTxsSession, oTestVm);
            fRc, _ = oCurTest.createSession('Test #%d' % (i,));
            if fRc is False:
                reporter.error('Test #%d failed: Could not create session' % (i,));
                break;
            try:
                oCurProgress = oCurTest.oTest.oGuest.updateGuestAdditions(oCurTest.sSrc, oCurTest.aArgs, oCurTest.fFlags);
                if oCurProgress is not None:
                    oProgress = vboxwrappers.ProgressWrapper(oCurProgress, self.oTstDrv.oVBoxMgr, self.oTstDrv, "gctrlUpGA");
                    try:
                        oProgress.wait();
                        if not oProgress.isSuccess():
                            oProgress.logResult(fIgnoreErrors = True);
                            fRc = False;
                    except:
                        reporter.logXcpt('Waiting exception for updating Guest Additions:');
                        fRc = False;
                else:
                    reporter.error('No progress object returned');
                    fRc = False;
            except:
                # Just log, don't assume an error here (will be done in the main loop then).
                reporter.logXcpt('Updating Guest Additions exception for sSrc="%s", fFlags="%s":' \
                                 % (oCurTest.sSrc, oCurTest.fFlags));
                fRc = False;
            oCurTest.closeSession();
            if fRc is oCurRes.fRc:
                if fRc:
                    ## @todo Verify if Guest Additions were really updated (build, revision, ...).
                    pass;
            else:
                reporter.error('Test #%d failed: Got %s, expected %s' % (i, fRc, oCurRes.fRc));
                fRc = False;
                break;

        return (fRc, oTxsSession);



class tdAddGuestCtrl(vbox.TestDriver):                                         # pylint: disable=too-many-instance-attributes,too-many-public-methods
    """
    Guest control using VBoxService on the guest.
    """

    def __init__(self):
        vbox.TestDriver.__init__(self);
        self.oTestVmSet = self.oTestVmManager.getSmokeVmSet('nat');
        self.asRsrcs    = None;
        self.fQuick     = False; # Don't skip lengthly tests by default.
        self.addSubTestDriver(SubTstDrvAddGuestCtrl(self));

    #
    # Overridden methods.
    #
    def showUsage(self):
        """
        Shows the testdriver usage.
        """
        rc = vbox.TestDriver.showUsage(self);
        reporter.log('');
        reporter.log('tdAddGuestCtrl Options:');
        reporter.log('  --quick');
        reporter.log('      Same as --virt-modes hwvirt --cpu-counts 1.');
        return rc;

    def parseOption(self, asArgs, iArg):                                        # pylint: disable=too-many-branches,too-many-statements
        """
        Parses the testdriver arguments from the command line.
        """
        if asArgs[iArg] == '--quick':
            self.parseOption(['--virt-modes', 'hwvirt'], 0);
            self.parseOption(['--cpu-counts', '1'], 0);
            self.fQuick = True;
        else:
            return vbox.TestDriver.parseOption(self, asArgs, iArg);
        return iArg + 1;

    def actionConfig(self):
        if not self.importVBoxApi(): # So we can use the constant below.
            return False;

        eNic0AttachType = vboxcon.NetworkAttachmentType_NAT;
        sGaIso = self.getGuestAdditionsIso();
        return self.oTestVmSet.actionConfig(self, eNic0AttachType = eNic0AttachType, sDvdImage = sGaIso);

    def actionExecute(self):
        return self.oTestVmSet.actionExecute(self, self.testOneCfg);

    #
    # Test execution helpers.
    #
    def testOneCfg(self, oVM, oTestVm): # pylint: disable=too-many-statements
        """
        Runs the specified VM thru the tests.

        Returns a success indicator on the general test execution. This is not
        the actual test result.
        """

        self.logVmInfo(oVM);

        fRc = True;
        oSession, oTxsSession = self.startVmAndConnectToTxsViaTcp(oTestVm.sVmName, fCdWait = False);
        reporter.log("TxsSession: %s" % (oTxsSession,));
        if oSession is not None:
            self.addTask(oTxsSession);

            fManual = False; # Manual override for local testing. (Committed version shall be False.)
            if not fManual:
                fRc, oTxsSession = self.aoSubTstDrvs[0].testIt(oTestVm, oSession, oTxsSession);
            else:
                fRc, oTxsSession = self.testGuestCtrlManual(oSession, oTxsSession, oTestVm);

            # Cleanup.
            self.removeTask(oTxsSession);
            if not fManual:
                self.terminateVmBySession(oSession);
        else:
            fRc = False;
        return fRc;

    def gctrlReportError(self, progress):
        """
        Helper function to report an error of a
        given progress object.
        """
        if progress is None:
            reporter.log('No progress object to print error for');
        else:
            errInfo = progress.errorInfo;
            if errInfo:
                reporter.log('%s' % (errInfo.text,));
        return False;

    def gctrlGetRemainingTime(self, msTimeout, msStart):
        """
        Helper function to return the remaining time (in ms)
        based from a timeout value and the start time (both in ms).
        """
        if msTimeout == 0:
            return 0xFFFFFFFE; # Wait forever.
        msElapsed = base.timestampMilli() - msStart;
        if msElapsed > msTimeout:
            return 0; # No time left.
        return msTimeout - msElapsed;

    def testGuestCtrlManual(self, oSession, oTxsSession, oTestVm):                # pylint: disable=too-many-locals,too-many-statements,unused-argument,unused-variable
        """
        For manually testing certain bits.
        """

        reporter.log('Manual testing ...');
        fRc = True;

        sUser = 'Administrator';
        sPassword = 'password';

        oGuest = oSession.o.console.guest;
        oGuestSession = oGuest.createSession(sUser,
                                             sPassword,
                                             "", "Manual Test");

        aWaitFor = [ vboxcon.GuestSessionWaitForFlag_Start ];
        _ = oGuestSession.waitForArray(aWaitFor, 30 * 1000);

        sCmd = SubTstDrvAddGuestCtrl.getGuestSystemShell(oTestVm);
        aArgs = [ sCmd, '/C', 'dir', '/S', 'c:\\windows' ];
        aEnv = [];
        fFlags = [];

        for _ in xrange(100):
            oProc = oGuestSession.processCreate(sCmd, aArgs if self.fpApiVer >= 5.0 else aArgs[1:],
                                                aEnv, fFlags, 30 * 1000);

            aWaitFor = [ vboxcon.ProcessWaitForFlag_Terminate ];
            _ = oProc.waitForArray(aWaitFor, 30 * 1000);

        oGuestSession.close();
        oGuestSession = None;

        time.sleep(5);

        oSession.o.console.PowerDown();

        return (fRc, oTxsSession);

if __name__ == '__main__':
    sys.exit(tdAddGuestCtrl().main(sys.argv));
