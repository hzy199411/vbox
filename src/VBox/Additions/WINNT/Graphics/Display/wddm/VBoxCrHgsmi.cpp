#include <VBox/VBoxCrHgsmi.h>
#include <iprt/err.h>

#include "VBoxUhgsmiKmt.h"

static VBOXDISPKMT_CALLBACKS g_VBoxCrHgsmiKmtCallbacks;
static int g_bVBoxKmtCallbacksInited = 0;

#ifdef VBOX_CRHGSMI_WITH_D3DDEV
static VBOXCRHGSMI_CALLBACKS g_VBoxCrHgsmiCallbacks;
static HMODULE g_hVBoxCrHgsmiProvider = NULL;
static uint32_t g_cVBoxCrHgsmiProvider = 0;


typedef VBOXWDDMDISP_DECL(int) FNVBOXDISPCRHGSMI_INIT(PVBOXCRHGSMI_CALLBACKS pCallbacks);
typedef FNVBOXDISPCRHGSMI_INIT *PFNVBOXDISPCRHGSMI_INIT;

typedef VBOXWDDMDISP_DECL(int) FNVBOXDISPCRHGSMI_TERM();
typedef FNVBOXDISPCRHGSMI_TERM *PFNVBOXDISPCRHGSMI_TERM;

typedef VBOXWDDMDISP_DECL(HVBOXCRHGSMI_CLIENT) FNVBOXDISPCRHGSMI_QUERY_CLIENT();
typedef FNVBOXDISPCRHGSMI_QUERY_CLIENT *PFNVBOXDISPCRHGSMI_QUERY_CLIENT;

static PFNVBOXDISPCRHGSMI_INIT g_pfnVBoxDispCrHgsmiInit = NULL;
static PFNVBOXDISPCRHGSMI_TERM g_pfnVBoxDispCrHgsmiTerm = NULL;
static PFNVBOXDISPCRHGSMI_QUERY_CLIENT g_pfnVBoxDispCrHgsmiQueryClient = NULL;

VBOXCRHGSMI_DECL(int) VBoxCrHgsmiInit(PVBOXCRHGSMI_CALLBACKS pCallbacks)
{
    if (!g_bVBoxKmtCallbacksInited)
    {
        HRESULT hr = vboxDispKmtCallbacksInit(&g_VBoxCrHgsmiKmtCallbacks);
        Assert(hr == S_OK);
        if (hr == S_OK)
            g_bVBoxKmtCallbacksInited = 1;
        else
            g_bVBoxKmtCallbacksInited = -1;
    }

    Assert(g_bVBoxKmtCallbacksInited);
    if (g_bVBoxKmtCallbacksInited < 0)
    {
        Assert(0);
        return VERR_NOT_SUPPORTED;
    }

    g_VBoxCrHgsmiCallbacks = *pCallbacks;
    if (!g_hVBoxCrHgsmiProvider)
    {
        BOOL bRc = GetModuleHandleEx(0, L"VBoxDispD3D", &g_hVBoxCrHgsmiProvider);
//        g_hVBoxCrHgsmiProvider = GetModuleHandle(L"VBoxDispD3D");
        if (bRc)
        {
            g_pfnVBoxDispCrHgsmiInit = (PFNVBOXDISPCRHGSMI_INIT)GetProcAddress(g_hVBoxCrHgsmiProvider, "VBoxDispCrHgsmiInit");
            Assert(g_pfnVBoxDispCrHgsmiInit);
            if (g_pfnVBoxDispCrHgsmiInit)
            {
                g_pfnVBoxDispCrHgsmiInit(pCallbacks);
            }

            g_pfnVBoxDispCrHgsmiTerm = (PFNVBOXDISPCRHGSMI_TERM)GetProcAddress(g_hVBoxCrHgsmiProvider, "VBoxDispCrHgsmiTerm");
            Assert(g_pfnVBoxDispCrHgsmiTerm);

            g_pfnVBoxDispCrHgsmiQueryClient = (PFNVBOXDISPCRHGSMI_QUERY_CLIENT)GetProcAddress(g_hVBoxCrHgsmiProvider, "VBoxDispCrHgsmiQueryClient");
            Assert(g_pfnVBoxDispCrHgsmiQueryClient);
        }
#ifdef DEBUG_misha
        else
        {
            DWORD winEr = GetLastError();
            Assert(0);
        }
#endif
    }

    if (g_hVBoxCrHgsmiProvider)
    {
        if (g_pfnVBoxDispCrHgsmiInit)
        {
            g_pfnVBoxDispCrHgsmiInit(pCallbacks);
        }
        ++g_cVBoxCrHgsmiProvider;
        return VINF_SUCCESS;
    }

    /* we're called from ogl ICD driver*/
    Assert(0);

    return VINF_SUCCESS;
}

static __declspec(thread) PVBOXUHGSMI_PRIVATE_KMT gt_pHgsmiGL = NULL;

VBOXCRHGSMI_DECL(HVBOXCRHGSMI_CLIENT) VBoxCrHgsmiQueryClient()
{

    HVBOXCRHGSMI_CLIENT hClient;
    if (g_pfnVBoxDispCrHgsmiQueryClient)
    {
        hClient = g_pfnVBoxDispCrHgsmiQueryClient();
//#ifdef DEBUG_misha
//        Assert(hClient);
//#endif
        if (hClient)
            return hClient;
    }
    PVBOXUHGSMI_PRIVATE_KMT pHgsmiGL = gt_pHgsmiGL;
    if (pHgsmiGL)
    {
        Assert(pHgsmiGL->BasePrivate.hClient);
        return pHgsmiGL->BasePrivate.hClient;
    }
    pHgsmiGL = (PVBOXUHGSMI_PRIVATE_KMT)RTMemAllocZ(sizeof (*pHgsmiGL));
    if (pHgsmiGL)
    {
#if 0
        HRESULT hr = vboxUhgsmiKmtCreate(pHgsmiGL, TRUE /* bD3D tmp for injection thread*/);
#else
        HRESULT hr = vboxUhgsmiKmtEscCreate(pHgsmiGL, TRUE /* bD3D tmp for injection thread*/);
#endif
        Assert(hr == S_OK);
        if (hr == S_OK)
        {
            hClient = g_VBoxCrHgsmiCallbacks.pfnClientCreate(&pHgsmiGL->BasePrivate.Base);
            Assert(hClient);
            if (hClient)
            {
                pHgsmiGL->BasePrivate.hClient = hClient;
                gt_pHgsmiGL = pHgsmiGL;
                return hClient;
            }
            vboxUhgsmiKmtDestroy(pHgsmiGL);
        }
        RTMemFree(pHgsmiGL);
    }

    return NULL;
}
#else
static int vboxCrHgsmiInitPerform(VBOXDISPKMT_CALLBACKS *pCallbacks)
{
    HRESULT hr = vboxDispKmtCallbacksInit(pCallbacks);
    /*Assert(hr == S_OK);*/
    if (hr == S_OK)
    {
        /* check if we can create the hgsmi */
        PVBOXUHGSMI pHgsmi = VBoxCrHgsmiCreate();
        if (pHgsmi)
        {
            /* yes, we can, so this is wddm mode */
            VBoxCrHgsmiDestroy(pHgsmi);
            Log(("CrHgsmi: WDDM mode supported\n"));
            return 1;
        }
        vboxDispKmtCallbacksTerm(pCallbacks);
    }
    Log(("CrHgsmi: unsupported\n"));
    return -1;
}

VBOXCRHGSMI_DECL(int) VBoxCrHgsmiInit()
{
    if (!g_bVBoxKmtCallbacksInited)
    {
        g_bVBoxKmtCallbacksInited = vboxCrHgsmiInitPerform(&g_VBoxCrHgsmiKmtCallbacks);
        Assert(g_bVBoxKmtCallbacksInited);
    }

    return g_bVBoxKmtCallbacksInited > 0 ? VINF_SUCCESS : VERR_NOT_SUPPORTED;
}

VBOXCRHGSMI_DECL(PVBOXUHGSMI) VBoxCrHgsmiCreate()
{
    PVBOXUHGSMI_PRIVATE_KMT pHgsmiGL = (PVBOXUHGSMI_PRIVATE_KMT)RTMemAllocZ(sizeof (*pHgsmiGL));
    if (pHgsmiGL)
    {
#if 0
        HRESULT hr = vboxUhgsmiKmtCreate(pHgsmiGL, TRUE /* bD3D tmp for injection thread*/);
#else
        HRESULT hr = vboxUhgsmiKmtEscCreate(pHgsmiGL, TRUE /* bD3D tmp for injection thread*/);
#endif
        Log(("CrHgsmi: faled to create KmtEsc UHGSMI instance, hr (0x%x)\n", hr));
        if (hr == S_OK)
        {
            return &pHgsmiGL->BasePrivate.Base;
        }
        RTMemFree(pHgsmiGL);
    }

    return NULL;
}

VBOXCRHGSMI_DECL(void) VBoxCrHgsmiDestroy(PVBOXUHGSMI pHgsmi)
{
    PVBOXUHGSMI_PRIVATE_KMT pHgsmiGL = VBOXUHGSMIKMT_GET(pHgsmi);
    HRESULT hr = vboxUhgsmiKmtDestroy(pHgsmiGL);
    Assert(hr == S_OK);
    if (hr == S_OK)
    {
        RTMemFree(pHgsmiGL);
    }
}
#endif

VBOXCRHGSMI_DECL(int) VBoxCrHgsmiTerm()
{
#if 0
    PVBOXUHGSMI_PRIVATE_KMT pHgsmiGL = gt_pHgsmiGL;
    if (pHgsmiGL)
    {
        g_VBoxCrHgsmiCallbacks.pfnClientDestroy(pHgsmiGL->BasePrivate.hClient);
        vboxUhgsmiKmtDestroy(pHgsmiGL);
        gt_pHgsmiGL = NULL;
    }

    if (g_pfnVBoxDispCrHgsmiTerm)
        g_pfnVBoxDispCrHgsmiTerm();
#endif
    if (g_bVBoxKmtCallbacksInited > 0)
    {
        vboxDispKmtCallbacksTerm(&g_VBoxCrHgsmiKmtCallbacks);
    }
    return VINF_SUCCESS;
}

VBOXCRHGSMI_DECL(void) VBoxCrHgsmiLog(char * szString)
{
    VBOXDISPKMT_ADAPTER Adapter;
    HRESULT hr = vboxDispKmtOpenAdapter(&g_VBoxCrHgsmiKmtCallbacks, &Adapter);
    Assert(hr == S_OK);
    if (hr == S_OK)
    {
        uint32_t cbString = (uint32_t)strlen(szString) + 1;
        uint32_t cbCmd = RT_OFFSETOF(VBOXDISPIFESCAPE_DBGPRINT, aStringBuf[cbString]);
        PVBOXDISPIFESCAPE_DBGPRINT pCmd = (PVBOXDISPIFESCAPE_DBGPRINT)RTMemAllocZ(cbCmd);
        Assert(pCmd);
        if (pCmd)
        {
            pCmd->EscapeHdr.escapeCode = VBOXESC_DBGPRINT;
            memcpy(pCmd->aStringBuf, szString, cbString);

            D3DKMT_ESCAPE EscapeData = {0};
            EscapeData.hAdapter = Adapter.hAdapter;
            //EscapeData.hDevice = NULL;
            EscapeData.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    //        EscapeData.Flags.HardwareAccess = 1;
            EscapeData.pPrivateDriverData = pCmd;
            EscapeData.PrivateDriverDataSize = cbCmd;
            //EscapeData.hContext = NULL;

            int Status = g_VBoxCrHgsmiKmtCallbacks.pfnD3DKMTEscape(&EscapeData);
            Assert(!Status);

            RTMemFree(pCmd);
        }
        hr = vboxDispKmtCloseAdapter(&Adapter);
        Assert(hr == S_OK);
    }
}

///* to be used by injection thread and by ogl ICD driver for hgsmi initialization*/
//VBOXCRHGSMI_DECL(int) VBoxCrHgsmiCustomCreate(PVBOXUHGSMI *ppHgsmi)
//{
//    PVBOXUHGSMI_PRIVATE_KMT pHgsmi = RTMemAllocZ(sizeof (*pHgsmi));
//    if (pHgsmi)
//    {
//        HRESULT hr = vboxUhgsmiKmtCreate(pHgsmi, FALSE);
//        Assert(hr == S_OK);
//        if (hr == S_OK)
//        {
//            *ppHgsmi = &pHgsmi->BasePrivate.Base;
//            return VINF_SUCCESS;
//        }
//        RTMemFree(pHgsmi);
//        return VERR_GENERAL_FAILURE;
//    }
//    return VERR_NO_MEMORY;
//}
//
//VBOXCRHGSMI_DECL(int) VBoxCrHgsmiCustomDestroy(PVBOXUHGSMI pHgsmi)
//{
//    PVBOXUHGSMI_PRIVATE_KMT pHgsmiKmt = VBOXUHGSMIKMT_GET(pHgsmi);
//    HRESULT hr = vboxUhgsmiKmtDestroy(pHgsmiKmt, FALSE);
//    Assert(hr == S_OK);
//    if (hr == S_OK)
//    {
//        RTMemFree(pHgsmiKmt);
//        return VINF_SUCCESS;
//    }
//    return VERR_GENERAL_FAILURE;
//}

