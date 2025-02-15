/*
 * Copyright 2015 Zhenbo Li
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "wine/debug.h"

#include "mshtml_private.h"
#include "htmlevent.h"
#include "mshtmdid.h"
#include "initguid.h"
#include "msxml6.h"
#include "objsafe.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

static HRESULT bstr_to_nsacstr(BSTR bstr, nsACString *str)
{
    char *cstr = strdupWtoU(bstr);
    if(!cstr)
        return E_OUTOFMEMORY;
    nsACString_Init(str, cstr);
    free(cstr);
    return S_OK;
}

static HRESULT variant_to_nsastr(VARIANT var, nsAString *ret)
{
    switch(V_VT(&var)) {
        case VT_NULL:
        case VT_ERROR:
        case VT_EMPTY:
            nsAString_Init(ret, NULL);
            return S_OK;
        case VT_BSTR:
            nsAString_InitDepend(ret, V_BSTR(&var));
            return S_OK;
        default:
            FIXME("Unsupported VARIANT: %s\n", debugstr_variant(&var));
            return E_INVALIDARG;
    }
}

static HRESULT return_nscstr(nsresult nsres, nsACString *nscstr, BSTR *p)
{
    const char *str;
    int len;

    if(NS_FAILED(nsres)) {
        ERR("failed: %08lx\n", nsres);
        nsACString_Finish(nscstr);
        return E_FAIL;
    }

    nsACString_GetData(nscstr, &str);

    if(*str) {
        len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
        *p = SysAllocStringLen(NULL, len - 1);
        if(!*p) {
            nsACString_Finish(nscstr);
            return E_OUTOFMEMORY;
        }
        MultiByteToWideChar(CP_UTF8, 0, str, -1, *p, len);
    }else {
        *p = NULL;
    }

    nsACString_Finish(nscstr);
    return S_OK;
}

static const eventid_t events[] = {
    EVENTID_READYSTATECHANGE,
    EVENTID_LOAD,
    EVENTID_LOADSTART,
    EVENTID_LOADEND,
    EVENTID_PROGRESS,
    EVENTID_ABORT,
    EVENTID_ERROR,
    EVENTID_TIMEOUT,
};

typedef enum {
    response_type_empty,
    response_type_text,
    response_type_doc,
    response_type_arraybuf,
    response_type_blob,
    response_type_stream
} response_type_t;

static const struct {
    const WCHAR *str;
    const WCHAR *nsxhr_str;
} response_type_desc[] = {
    [response_type_empty]       = { L"",            L"" },
    [response_type_text]        = { L"text",        L"" },
    [response_type_doc]         = { L"document",    L"" }, /* FIXME */
    [response_type_arraybuf]    = { L"arraybuffer", L"arraybuffer" },
    [response_type_blob]        = { L"blob",        L"arraybuffer" },
    [response_type_stream]      = { L"ms-stream",   L"arraybuffer" } /* FIXME */
};

typedef struct {
    nsIDOMEventListener nsIDOMEventListener_iface;
    LONG ref;
    EventTarget *event_target;
    HTMLInnerWindow *window;
    nsIXMLHttpRequest *nsxhr;
    DWORD events_mask;
} XMLHttpReqEventListener;

struct HTMLXMLHttpRequest {
    EventTarget event_target;
    IHTMLXMLHttpRequest IHTMLXMLHttpRequest_iface;
    IHTMLXMLHttpRequest2 IHTMLXMLHttpRequest2_iface;
    IWineXMLHttpRequestPrivate IWineXMLHttpRequestPrivate_iface;
    IProvideClassInfo2 IProvideClassInfo2_iface;
    LONG ref;
    document_type_t doctype_override;
    response_type_t response_type;
    IDispatch *response_obj;
    HTMLInnerWindow *window;
    nsIXMLHttpRequest *nsxhr;
    XMLHttpReqEventListener *event_listener;
};

typedef struct {
    EventTarget event_target;
    IHTMLXDomainRequest IHTMLXDomainRequest_iface;
    LONG ref;
    HTMLInnerWindow *window;
    nsIXMLHttpRequest *nsxhr;
    XMLHttpReqEventListener *event_listener;
} HTMLXDomainRequest;

static void detach_xhr_event_listener(XMLHttpReqEventListener *event_listener)
{
    nsIDOMEventTarget *event_target;
    DWORD events_mask, i;
    nsAString str;
    nsresult nsres;

    nsres = nsIXMLHttpRequest_QueryInterface(event_listener->nsxhr, &IID_nsIDOMEventTarget, (void**)&event_target);
    assert(nsres == NS_OK);

    for(events_mask = event_listener->events_mask, i = 0; events_mask; events_mask >>= 1, i++) {
        if(!(events_mask & 1))
            continue;
        nsAString_InitDepend(&str, get_event_name(events[i]));
        nsres = nsIDOMEventTarget_RemoveEventListener(event_target, &str, &event_listener->nsIDOMEventListener_iface, FALSE);
        nsAString_Finish(&str);
        assert(nsres == NS_OK);
    }

    nsIDOMEventTarget_Release(event_target);

    event_listener->event_target = NULL;
    nsIDOMEventListener_Release(&event_listener->nsIDOMEventListener_iface);
}

static HRESULT WINAPI nsxhr_send(nsIXMLHttpRequest *nsxhr, VARIANT body)
{
    nsIWritableVariant *nsbody = NULL;
    nsresult nsres = NS_OK;

    switch(V_VT(&body)) {
    case VT_NULL:
    case VT_EMPTY:
    case VT_ERROR:
        break;
    case VT_BSTR: {
        nsAString nsstr;

        nsbody = create_nsvariant();
        if(!nsbody)
            return E_OUTOFMEMORY;

        nsAString_InitDepend(&nsstr, V_BSTR(&body));
        nsres = nsIWritableVariant_SetAsAString(nsbody, &nsstr);
        nsAString_Finish(&nsstr);
        break;
    }
    default:
        FIXME("unsupported body type %s\n", debugstr_variant(&body));
        return E_NOTIMPL;
    }

    if(NS_SUCCEEDED(nsres))
        nsres = nsIXMLHttpRequest_Send(nsxhr, (nsIVariant*)nsbody);
    if(nsbody)
        nsIWritableVariant_Release(nsbody);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_Send failed: %08lx\n", nsres);
        return map_nsresult(nsres);
    }

    return S_OK;
}


static inline XMLHttpReqEventListener *impl_from_nsIDOMEventListener(nsIDOMEventListener *iface)
{
    return CONTAINING_RECORD(iface, XMLHttpReqEventListener, nsIDOMEventListener_iface);
}

static nsresult NSAPI XMLHttpReqEventListener_QueryInterface(nsIDOMEventListener *iface,
        nsIIDRef riid, void **result)
{
    XMLHttpReqEventListener *This = impl_from_nsIDOMEventListener(iface);

    if(IsEqualGUID(&IID_nsISupports, riid)) {
        TRACE("(%p)->(IID_nsISupports, %p)\n", This, result);
        *result = &This->nsIDOMEventListener_iface;
    }else if(IsEqualGUID(&IID_nsIDOMEventListener, riid)) {
        TRACE("(%p)->(IID_nsIDOMEventListener %p)\n", This, result);
        *result = &This->nsIDOMEventListener_iface;
    }else {
        *result = NULL;
        TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), result);
        return NS_NOINTERFACE;
    }

    nsIDOMEventListener_AddRef(&This->nsIDOMEventListener_iface);
    return NS_OK;
}

static nsrefcnt NSAPI XMLHttpReqEventListener_AddRef(nsIDOMEventListener *iface)
{
    XMLHttpReqEventListener *This = impl_from_nsIDOMEventListener(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static nsrefcnt NSAPI XMLHttpReqEventListener_Release(nsIDOMEventListener *iface)
{
    XMLHttpReqEventListener *This = impl_from_nsIDOMEventListener(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        assert(!This->event_target);
        free(This);
    }

    return ref;
}

static nsresult NSAPI XMLHttpReqEventListener_HandleEvent(nsIDOMEventListener *iface, nsIDOMEvent *nsevent)
{
    XMLHttpReqEventListener *This = impl_from_nsIDOMEventListener(iface);
    DOMEvent *event;
    HRESULT hres;

    TRACE("(%p)\n", This);

    if(!This->event_target)
        return NS_OK;

    hres = create_event_from_nsevent(nsevent, This->window, dispex_compat_mode(&This->event_target->dispex), &event);
    if(SUCCEEDED(hres) ){
        dispatch_event(This->event_target, event);
        IDOMEvent_Release(&event->IDOMEvent_iface);
    }
    return NS_OK;
}

static const nsIDOMEventListenerVtbl XMLHttpReqEventListenerVtbl = {
    XMLHttpReqEventListener_QueryInterface,
    XMLHttpReqEventListener_AddRef,
    XMLHttpReqEventListener_Release,
    XMLHttpReqEventListener_HandleEvent
};

static inline HTMLXMLHttpRequest *impl_from_IHTMLXMLHttpRequest(IHTMLXMLHttpRequest *iface)
{
    return CONTAINING_RECORD(iface, HTMLXMLHttpRequest, IHTMLXMLHttpRequest_iface);
}

static HRESULT WINAPI HTMLXMLHttpRequest_QueryInterface(IHTMLXMLHttpRequest *iface, REFIID riid, void **ppv)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        *ppv = &This->IHTMLXMLHttpRequest_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        *ppv = &This->IHTMLXMLHttpRequest_iface;
    }else if(IsEqualGUID(&IID_IHTMLXMLHttpRequest, riid)) {
        *ppv = &This->IHTMLXMLHttpRequest_iface;
    }else if(IsEqualGUID(&IID_IHTMLXMLHttpRequest2, riid)) {
        *ppv = &This->IHTMLXMLHttpRequest2_iface;
    }else if(IsEqualGUID(&IID_IWineXMLHttpRequestPrivate, riid)) {
        *ppv = &This->IWineXMLHttpRequestPrivate_iface;
    }else if(IsEqualGUID(&IID_IProvideClassInfo, riid)) {
        *ppv = &This->IProvideClassInfo2_iface;
    }else if(IsEqualGUID(&IID_IProvideClassInfo2, riid)) {
        *ppv = &This->IProvideClassInfo2_iface;
    }else {
        return EventTarget_QI(&This->event_target, riid, ppv);
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI HTMLXMLHttpRequest_AddRef(IHTMLXMLHttpRequest *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLXMLHttpRequest_Release(IHTMLXMLHttpRequest *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        if(This->response_obj)
            IDispatch_Release(This->response_obj);
        if(This->event_listener)
            detach_xhr_event_listener(This->event_listener);
        IHTMLWindow2_Release(&This->window->base.IHTMLWindow2_iface);
        release_event_target(&This->event_target);
        release_dispex(&This->event_target.dispex);
        nsIXMLHttpRequest_Release(This->nsxhr);
        free(This);
    }

    return ref;
}

static HRESULT WINAPI HTMLXMLHttpRequest_GetTypeInfoCount(IHTMLXMLHttpRequest *iface, UINT *pctinfo)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLXMLHttpRequest_GetTypeInfo(IHTMLXMLHttpRequest *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLXMLHttpRequest_GetIDsOfNames(IHTMLXMLHttpRequest *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLXMLHttpRequest_Invoke(IHTMLXMLHttpRequest *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_readyState(IHTMLXMLHttpRequest *iface, LONG *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    UINT16 val;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;
    nsres = nsIXMLHttpRequest_GetReadyState(This->nsxhr, &val);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_GetReadyState failed: %08lx\n", nsres);
        return E_FAIL;
    }
    *p = val;
    return S_OK;
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_responseBody(IHTMLXMLHttpRequest *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_responseText(IHTMLXMLHttpRequest *iface, BSTR *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    nsAString_Init(&nsstr, NULL);
    nsres = nsIXMLHttpRequest_GetResponseText(This->nsxhr, &nsstr);
    return return_nsstr(nsres, &nsstr, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_responseXML(IHTMLXMLHttpRequest *iface, IDispatch **p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    IXMLDOMDocument *xmldoc = NULL;
    BSTR str;
    HRESULT hres;
    VARIANT_BOOL vbool;
    IObjectSafety *safety;

    TRACE("(%p)->(%p)\n", This, p);

    if(dispex_compat_mode(&This->event_target.dispex) >= COMPAT_MODE_IE10) {
        nsACString header, nscstr;
        document_type_t doctype;
        HTMLDocumentNode *doc;
        nsIDOMDocument *nsdoc;
        const char *type;
        nsresult nsres;

        nsres = nsIXMLHttpRequest_GetResponseXML(This->nsxhr, &nsdoc);
        if(NS_FAILED(nsres))
            return map_nsresult(nsres);
        if(!nsdoc) {
            *p = NULL;
            return S_OK;
        }

        if(This->doctype_override != DOCTYPE_INVALID)
            doctype = This->doctype_override;
        else {
            doctype = DOCTYPE_XML;
            nsACString_InitDepend(&header, "Content-Type");
            nsACString_InitDepend(&nscstr, NULL);
            nsres = nsIXMLHttpRequest_GetResponseHeader(This->nsxhr, &header, &nscstr);
            nsACString_Finish(&header);
            if(NS_SUCCEEDED(nsres)) {
                nsACString_GetData(&nscstr, &type);
                if(!stricmp(type, "application/xhtml+xml"))
                    doctype = DOCTYPE_XHTML;
                else if(!stricmp(type, "image/svg+xml"))
                    doctype = DOCTYPE_SVG;
            }
            nsACString_Finish(&nscstr);
        }

        hres = create_document_node(nsdoc, This->window->base.outer_window->browser, NULL, doctype,
                                    dispex_compat_mode(&This->window->event_target.dispex), &doc);
        nsIDOMDocument_Release(nsdoc);
        if(FAILED(hres))
            return hres;

        *p = (IDispatch*)&doc->IHTMLDocument2_iface;
        return S_OK;
    }

    hres = CoCreateInstance(&CLSID_DOMDocument, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument, (void**)&xmldoc);
    if(FAILED(hres)) {
        ERR("CoCreateInstance failed: %08lx\n", hres);
        return hres;
    }

    hres = IHTMLXMLHttpRequest_get_responseText(iface, &str);
    if(FAILED(hres)) {
        IXMLDOMDocument_Release(xmldoc);
        ERR("get_responseText failed: %08lx\n", hres);
        return hres;
    }

    hres = IXMLDOMDocument_loadXML(xmldoc, str, &vbool);
    SysFreeString(str);
    if(hres != S_OK || vbool != VARIANT_TRUE)
        WARN("loadXML failed: %08lx, returning an empty xmldoc\n", hres);

    hres = IXMLDOMDocument_QueryInterface(xmldoc, &IID_IObjectSafety, (void**)&safety);
    assert(SUCCEEDED(hres));
    hres = IObjectSafety_SetInterfaceSafetyOptions(safety, NULL,
        INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA | INTERFACE_USES_SECURITY_MANAGER,
        INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA | INTERFACE_USES_SECURITY_MANAGER);
    assert(SUCCEEDED(hres));
    IObjectSafety_Release(safety);

    *p = (IDispatch*)xmldoc;
    return S_OK;
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_status(IHTMLXMLHttpRequest *iface, LONG *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    UINT32 val;
    nsresult nsres;
    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    nsres = nsIXMLHttpRequest_GetStatus(This->nsxhr, &val);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_GetStatus failed: %08lx\n", nsres);
        return E_FAIL;
    }
    *p = val;
    if(val == 0)
        return E_FAIL; /* WinAPI thinks this is an error */

    return S_OK;
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_statusText(IHTMLXMLHttpRequest *iface, BSTR *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    nsACString nscstr;
    nsresult nsres;
    HRESULT hres;
    LONG state;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    hres = IHTMLXMLHttpRequest_get_readyState(iface, &state);
    if(FAILED(hres))
        return hres;

    if(state < 2) {
        *p = NULL;
        return E_FAIL;
    }

    nsACString_Init(&nscstr, NULL);
    nsres = nsIXMLHttpRequest_GetStatusText(This->nsxhr, &nscstr);
    return return_nscstr(nsres, &nscstr, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_put_onreadystatechange(IHTMLXMLHttpRequest *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_READYSTATECHANGE, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_get_onreadystatechange(IHTMLXMLHttpRequest *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_READYSTATECHANGE, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_abort(IHTMLXMLHttpRequest *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    nsresult nsres;

    TRACE("(%p)->()\n", This);

    nsres = nsIXMLHttpRequest_SlowAbort(This->nsxhr);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_SlowAbort failed: %08lx\n", nsres);
        return E_FAIL;
    }

    return S_OK;
}

static HRESULT HTMLXMLHttpRequest_open_hook(DispatchEx *dispex, WORD flags,
        DISPPARAMS *dp, VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    /* If only two arguments were given, implicitly set async to false */
    if((flags & DISPATCH_METHOD) && dp->cArgs == 2 && !dp->cNamedArgs) {
        VARIANT args[5];
        DISPPARAMS new_dp = {args, NULL, ARRAY_SIZE(args), 0};
        V_VT(args) = VT_EMPTY;
        V_VT(args+1) = VT_EMPTY;
        V_VT(args+2) = VT_BOOL;
        V_BOOL(args+2) = VARIANT_TRUE;
        args[3] = dp->rgvarg[0];
        args[4] = dp->rgvarg[1];

        TRACE("implicit async\n");

        return dispex_call_builtin(dispex, DISPID_IHTMLXMLHTTPREQUEST_OPEN, &new_dp, res, ei, caller);
    }

    return S_FALSE; /* fallback to default */
}

static HRESULT WINAPI HTMLXMLHttpRequest_open(IHTMLXMLHttpRequest *iface, BSTR bstrMethod, BSTR bstrUrl, VARIANT varAsync, VARIANT varUser, VARIANT varPassword)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    nsACString method, url;
    nsAString user, password;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%s %s %s %s %s)\n", This, debugstr_w(bstrMethod), debugstr_w(bstrUrl), debugstr_variant(&varAsync), debugstr_variant(&varUser), debugstr_variant(&varPassword));

    if(V_VT(&varAsync) != VT_BOOL) {
        LCID lcid = MAKELCID(MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US),SORT_DEFAULT);
        hres = VariantChangeTypeEx(&varAsync, &varAsync, lcid, 0, VT_BOOL);
        if(FAILED(hres)) {
            WARN("Failed to convert varAsync to BOOL: %#lx\n", hres);
            return hres;
        }
    }

    /* Note: Starting with Gecko 30.0 (Firefox 30.0 / Thunderbird 30.0 / SeaMonkey 2.27),
     * synchronous requests on the main thread have been deprecated due to the negative
     * effects to the user experience.
     */
    if(!V_BOOL(&varAsync)) {
        FIXME("Synchronous request is not supported yet\n");
        return E_FAIL;
    }

    hres = variant_to_nsastr(varUser, &user);
    if(FAILED(hres))
        return hres;
    hres = variant_to_nsastr(varPassword, &password);
    if(FAILED(hres)) {
        nsAString_Finish(&user);
        return hres;
    }

    hres = bstr_to_nsacstr(bstrMethod, &method);
    if(FAILED(hres)) {
        nsAString_Finish(&user);
        nsAString_Finish(&password);
        return hres;
    }
    hres = bstr_to_nsacstr(bstrUrl, &url);
    if(FAILED(hres)) {
        nsAString_Finish(&user);
        nsAString_Finish(&password);
        nsACString_Finish(&method);
        return hres;
    }

    nsres = nsIXMLHttpRequest_Open(This->nsxhr, &method, &url, TRUE,
            &user, &password, 0);

    nsACString_Finish(&method);
    nsACString_Finish(&url);
    nsAString_Finish(&user);
    nsAString_Finish(&password);

    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_Open failed: %08lx\n", nsres);
        return E_FAIL;
    }

    return S_OK;
}

static HRESULT WINAPI HTMLXMLHttpRequest_send(IHTMLXMLHttpRequest *iface, VARIANT varBody)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&varBody));

    return nsxhr_send(This->nsxhr, varBody);
}

static HRESULT WINAPI HTMLXMLHttpRequest_getAllResponseHeaders(IHTMLXMLHttpRequest *iface, BSTR *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    nsACString nscstr;
    nsresult nsres;
    HRESULT hres;
    LONG state;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    hres = IHTMLXMLHttpRequest_get_readyState(iface, &state);
    if(FAILED(hres))
        return hres;

    if(state < 2) {
        *p = NULL;
        return E_FAIL;
    }

    nsACString_Init(&nscstr, NULL);
    nsres = nsIXMLHttpRequest_GetAllResponseHeaders(This->nsxhr, &nscstr);
    return return_nscstr(nsres, &nscstr, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_getResponseHeader(IHTMLXMLHttpRequest *iface, BSTR bstrHeader, BSTR *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    nsACString header, ret;
    char *cstr;
    nsresult nsres;
    HRESULT hres;
    LONG state;
    TRACE("(%p)->(%s %p)\n", This, debugstr_w(bstrHeader), p);

    if(!p)
        return E_POINTER;
    if(!bstrHeader)
        return E_INVALIDARG;

    hres = IHTMLXMLHttpRequest_get_readyState(iface, &state);
    if(FAILED(hres))
        return hres;

    if(state < 2) {
        *p = NULL;
        return E_FAIL;
    }

    cstr = strdupWtoU(bstrHeader);
    nsACString_InitDepend(&header, cstr);
    nsACString_Init(&ret, NULL);

    nsres = nsIXMLHttpRequest_GetResponseHeader(This->nsxhr, &header, &ret);

    nsACString_Finish(&header);
    free(cstr);
    return return_nscstr(nsres, &ret, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_setRequestHeader(IHTMLXMLHttpRequest *iface, BSTR bstrHeader, BSTR bstrValue)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest(iface);
    char *header_u, *value_u;
    nsACString header, value;
    nsresult nsres;

    TRACE("(%p)->(%s %s)\n", This, debugstr_w(bstrHeader), debugstr_w(bstrValue));

    header_u = strdupWtoU(bstrHeader);
    if(bstrHeader && !header_u)
        return E_OUTOFMEMORY;

    value_u = strdupWtoU(bstrValue);
    if(bstrValue && !value_u) {
        free(header_u);
        return E_OUTOFMEMORY;
    }

    nsACString_InitDepend(&header, header_u);
    nsACString_InitDepend(&value, value_u);
    nsres = nsIXMLHttpRequest_SetRequestHeader(This->nsxhr, &header, &value);
    nsACString_Finish(&header);
    nsACString_Finish(&value);
    free(header_u);
    free(value_u);
    if(NS_FAILED(nsres)) {
        ERR("SetRequestHeader failed: %08lx\n", nsres);
        return E_FAIL;
    }

    return S_OK;
}

static const IHTMLXMLHttpRequestVtbl HTMLXMLHttpRequestVtbl = {
    HTMLXMLHttpRequest_QueryInterface,
    HTMLXMLHttpRequest_AddRef,
    HTMLXMLHttpRequest_Release,
    HTMLXMLHttpRequest_GetTypeInfoCount,
    HTMLXMLHttpRequest_GetTypeInfo,
    HTMLXMLHttpRequest_GetIDsOfNames,
    HTMLXMLHttpRequest_Invoke,
    HTMLXMLHttpRequest_get_readyState,
    HTMLXMLHttpRequest_get_responseBody,
    HTMLXMLHttpRequest_get_responseText,
    HTMLXMLHttpRequest_get_responseXML,
    HTMLXMLHttpRequest_get_status,
    HTMLXMLHttpRequest_get_statusText,
    HTMLXMLHttpRequest_put_onreadystatechange,
    HTMLXMLHttpRequest_get_onreadystatechange,
    HTMLXMLHttpRequest_abort,
    HTMLXMLHttpRequest_open,
    HTMLXMLHttpRequest_send,
    HTMLXMLHttpRequest_getAllResponseHeaders,
    HTMLXMLHttpRequest_getResponseHeader,
    HTMLXMLHttpRequest_setRequestHeader
};

static inline HTMLXMLHttpRequest *impl_from_IHTMLXMLHttpRequest2(IHTMLXMLHttpRequest2 *iface)
{
    return CONTAINING_RECORD(iface, HTMLXMLHttpRequest, IHTMLXMLHttpRequest2_iface);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_QueryInterface(IHTMLXMLHttpRequest2 *iface, REFIID riid, void **ppv)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IHTMLXMLHttpRequest_QueryInterface(&This->IHTMLXMLHttpRequest_iface, riid, ppv);
}

static ULONG WINAPI HTMLXMLHttpRequest2_AddRef(IHTMLXMLHttpRequest2 *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IHTMLXMLHttpRequest_AddRef(&This->IHTMLXMLHttpRequest_iface);
}

static ULONG WINAPI HTMLXMLHttpRequest2_Release(IHTMLXMLHttpRequest2 *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IHTMLXMLHttpRequest_Release(&This->IHTMLXMLHttpRequest_iface);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_GetTypeInfoCount(IHTMLXMLHttpRequest2 *iface, UINT *pctinfo)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_GetTypeInfo(IHTMLXMLHttpRequest2 *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_GetIDsOfNames(IHTMLXMLHttpRequest2 *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_Invoke(IHTMLXMLHttpRequest2 *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_put_timeout(IHTMLXMLHttpRequest2 *iface, LONG v)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);

    TRACE("(%p)->(%ld)\n", This, v);

    if(v < 0)
        return E_INVALIDARG;
    return map_nsresult(nsIXMLHttpRequest_SetTimeout(This->nsxhr, v));
}

static HRESULT WINAPI HTMLXMLHttpRequest2_get_timeout(IHTMLXMLHttpRequest2 *iface, LONG *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);
    nsresult nsres;
    UINT32 timeout;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    nsres = nsIXMLHttpRequest_GetTimeout(This->nsxhr, &timeout);
    *p = timeout;
    return map_nsresult(nsres);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_put_ontimeout(IHTMLXMLHttpRequest2 *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_TIMEOUT, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest2_get_ontimeout(IHTMLXMLHttpRequest2 *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IHTMLXMLHttpRequest2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_TIMEOUT, p);
}

static const IHTMLXMLHttpRequest2Vtbl HTMLXMLHttpRequest2Vtbl = {
    HTMLXMLHttpRequest2_QueryInterface,
    HTMLXMLHttpRequest2_AddRef,
    HTMLXMLHttpRequest2_Release,
    HTMLXMLHttpRequest2_GetTypeInfoCount,
    HTMLXMLHttpRequest2_GetTypeInfo,
    HTMLXMLHttpRequest2_GetIDsOfNames,
    HTMLXMLHttpRequest2_Invoke,
    HTMLXMLHttpRequest2_put_timeout,
    HTMLXMLHttpRequest2_get_timeout,
    HTMLXMLHttpRequest2_put_ontimeout,
    HTMLXMLHttpRequest2_get_ontimeout
};

static inline HTMLXMLHttpRequest *impl_from_IWineXMLHttpRequestPrivate(IWineXMLHttpRequestPrivate *iface)
{
    return CONTAINING_RECORD(iface, HTMLXMLHttpRequest, IWineXMLHttpRequestPrivate_iface);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_QueryInterface(IWineXMLHttpRequestPrivate *iface, REFIID riid, void **ppv)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IHTMLXMLHttpRequest_QueryInterface(&This->IHTMLXMLHttpRequest_iface, riid, ppv);
}

static ULONG WINAPI HTMLXMLHttpRequest_private_AddRef(IWineXMLHttpRequestPrivate *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IHTMLXMLHttpRequest_AddRef(&This->IHTMLXMLHttpRequest_iface);
}

static ULONG WINAPI HTMLXMLHttpRequest_private_Release(IWineXMLHttpRequestPrivate *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IHTMLXMLHttpRequest_Release(&This->IHTMLXMLHttpRequest_iface);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_GetTypeInfoCount(IWineXMLHttpRequestPrivate *iface, UINT *pctinfo)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_GetTypeInfo(IWineXMLHttpRequestPrivate *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_GetIDsOfNames(IWineXMLHttpRequestPrivate *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_Invoke(IWineXMLHttpRequestPrivate *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_response(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    IWineDispatchProxyCbPrivate *proxy;
    HRESULT hres = S_OK;
    UINT32 buf_size;
    nsresult nsres;
    UINT16 state;
    void *buf;

    TRACE("(%p)->(%p)\n", This, p);

    if(This->response_obj) {
        V_VT(p) = VT_DISPATCH;
        V_DISPATCH(p) = This->response_obj;
        IDispatch_AddRef(This->response_obj);
        return S_OK;
    }

    switch(This->response_type) {
    case response_type_empty:
    case response_type_text:
        hres = IHTMLXMLHttpRequest_get_responseText(&This->IHTMLXMLHttpRequest_iface, &V_BSTR(p));
        if(SUCCEEDED(hres))
            V_VT(p) = VT_BSTR;
        break;

    case response_type_doc:
        FIXME("response_type_doc\n");
        return E_NOTIMPL;

    case response_type_arraybuf:
    case response_type_blob:
        nsres = nsIXMLHttpRequest_GetReadyState(This->nsxhr, &state);
        if(NS_FAILED(nsres) || state < 4) {
            V_VT(p) = VT_EMPTY;
            break;
        }
        if(!(proxy = This->event_target.dispex.proxy)) {
            FIXME("No proxy\n");
            return E_NOTIMPL;
        }
        nsres = nsIXMLHttpRequest_GetResponseBuffer(This->nsxhr, NULL, 0, &buf_size);
        assert(nsres == NS_OK);

        if(This->response_type == response_type_arraybuf) {
            hres = proxy->lpVtbl->CreateArrayBuffer(proxy, buf_size, &This->response_obj, &buf);
            if(SUCCEEDED(hres)) {
                nsres = nsIXMLHttpRequest_GetResponseBuffer(This->nsxhr, buf, buf_size, &buf_size);
                assert(nsres == NS_OK);
            }
            break;
        }

        FIXME("response_type_blob\n");
        return E_NOTIMPL;

    case response_type_stream:
        FIXME("response_type_stream\n");
        return E_NOTIMPL;

    default:
        assert(0);
    }

    if(SUCCEEDED(hres) && This->response_obj) {
        V_VT(p) = VT_DISPATCH;
        V_DISPATCH(p) = This->response_obj;
        IDispatch_AddRef(This->response_obj);
    }
    return hres;
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_responseType(IWineXMLHttpRequestPrivate *iface, BSTR v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    nsAString nsstr;
    nsresult nsres;
    HRESULT hres;
    unsigned i;
    LONG state;

    TRACE("(%p)->(%s)\n", This, debugstr_w(v));

    hres = IHTMLXMLHttpRequest_get_readyState(&This->IHTMLXMLHttpRequest_iface, &state);
    if(FAILED(hres))
        return hres;

    if(state < 1 || state > 2) {
        /* FIXME: Return InvalidStateError */
        return E_FAIL;
    }

    for(i = 0; i < ARRAY_SIZE(response_type_desc); i++)
        if(!wcscmp(v, response_type_desc[i].str))
            break;
    if(i >= ARRAY_SIZE(response_type_desc))
        return S_OK;

    nsAString_InitDepend(&nsstr, response_type_desc[i].nsxhr_str);
    nsres = nsIXMLHttpRequest_SetResponseType(This->nsxhr, &nsstr);
    nsAString_Finish(&nsstr);
    if(NS_FAILED(nsres))
        return map_nsresult(nsres);

    This->response_type = i;
    return S_OK;
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_responseType(IWineXMLHttpRequestPrivate *iface, BSTR *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    *p = SysAllocString(response_type_desc[This->response_type].str);
    return *p ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_upload(IWineXMLHttpRequestPrivate *iface, IDispatch **p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    FIXME("(%p)->(%p)\n", This, p);

    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_withCredentials(IWineXMLHttpRequestPrivate *iface, VARIANT_BOOL v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%x)\n", This, v);

    return map_nsresult(nsIXMLHttpRequest_SetWithCredentials(This->nsxhr, !!v));
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_withCredentials(IWineXMLHttpRequestPrivate *iface, VARIANT_BOOL *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    nsresult nsres;
    cpp_bool b;

    TRACE("(%p)->(%p)\n", This, p);

    nsres = nsIXMLHttpRequest_GetWithCredentials(This->nsxhr, &b);
    if(NS_FAILED(nsres))
        return map_nsresult(nsres);
    *p = b ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_overrideMimeType(IWineXMLHttpRequestPrivate *iface, BSTR mimeType)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);
    static const WCHAR generic_type[] = L"application/octet-stream";
    document_type_t doctype = DOCTYPE_XML;
    const WCHAR *type = NULL;
    WCHAR *lowercase = NULL;
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%s)\n", This, debugstr_w(mimeType));

    if(mimeType) {
        if(mimeType[0]) {
            if(!(lowercase = wcsdup(mimeType)))
                return E_OUTOFMEMORY;
            _wcslwr(lowercase);
            type = lowercase;

            if(!wcscmp(type, L"application/xhtml+xml"))
                doctype = DOCTYPE_XHTML;
            else if(!wcscmp(type, L"image/svg+xml"))
                doctype = DOCTYPE_SVG;
        }else
            type = generic_type;
    }

    nsAString_InitDepend(&nsstr, type);
    nsres = nsIXMLHttpRequest_SlowOverrideMimeType(This->nsxhr, &nsstr);
    nsAString_Finish(&nsstr);
    free(lowercase);
    if(NS_SUCCEEDED(nsres))
        This->doctype_override = doctype;
    return map_nsresult(nsres);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_onerror(IWineXMLHttpRequestPrivate *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_ERROR, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_onerror(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_ERROR, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_onabort(IWineXMLHttpRequestPrivate *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_ABORT, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_onabort(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_ABORT, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_onprogress(IWineXMLHttpRequestPrivate *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_PROGRESS, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_onprogress(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_PROGRESS, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_onloadstart(IWineXMLHttpRequestPrivate *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_LOADSTART, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_onloadstart(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_LOADSTART, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_onloadend(IWineXMLHttpRequestPrivate *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_LOADEND, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_onloadend(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_LOADEND, p);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_put_onload(IWineXMLHttpRequestPrivate *iface, VARIANT v)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_LOAD, &v);
}

static HRESULT WINAPI HTMLXMLHttpRequest_private_get_onload(IWineXMLHttpRequestPrivate *iface, VARIANT *p)
{
    HTMLXMLHttpRequest *This = impl_from_IWineXMLHttpRequestPrivate(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_LOAD, p);
}

static const IWineXMLHttpRequestPrivateVtbl WineXMLHttpRequestPrivateVtbl = {
    HTMLXMLHttpRequest_private_QueryInterface,
    HTMLXMLHttpRequest_private_AddRef,
    HTMLXMLHttpRequest_private_Release,
    HTMLXMLHttpRequest_private_GetTypeInfoCount,
    HTMLXMLHttpRequest_private_GetTypeInfo,
    HTMLXMLHttpRequest_private_GetIDsOfNames,
    HTMLXMLHttpRequest_private_Invoke,
    HTMLXMLHttpRequest_private_get_response,
    HTMLXMLHttpRequest_private_put_responseType,
    HTMLXMLHttpRequest_private_get_responseType,
    HTMLXMLHttpRequest_private_get_upload,
    HTMLXMLHttpRequest_private_put_withCredentials,
    HTMLXMLHttpRequest_private_get_withCredentials,
    HTMLXMLHttpRequest_private_overrideMimeType,
    HTMLXMLHttpRequest_private_put_onerror,
    HTMLXMLHttpRequest_private_get_onerror,
    HTMLXMLHttpRequest_private_put_onabort,
    HTMLXMLHttpRequest_private_get_onabort,
    HTMLXMLHttpRequest_private_put_onprogress,
    HTMLXMLHttpRequest_private_get_onprogress,
    HTMLXMLHttpRequest_private_put_onloadstart,
    HTMLXMLHttpRequest_private_get_onloadstart,
    HTMLXMLHttpRequest_private_put_onloadend,
    HTMLXMLHttpRequest_private_get_onloadend,
    HTMLXMLHttpRequest_private_put_onload,
    HTMLXMLHttpRequest_private_get_onload
};

static inline HTMLXMLHttpRequest *impl_from_IProvideClassInfo2(IProvideClassInfo2 *iface)
{
    return CONTAINING_RECORD(iface, HTMLXMLHttpRequest, IProvideClassInfo2_iface);
}

static HRESULT WINAPI ProvideClassInfo_QueryInterface(IProvideClassInfo2 *iface, REFIID riid, void **ppv)
{
    HTMLXMLHttpRequest *This = impl_from_IProvideClassInfo2(iface);
    return IHTMLXMLHttpRequest_QueryInterface(&This->IHTMLXMLHttpRequest_iface, riid, ppv);
}

static ULONG WINAPI ProvideClassInfo_AddRef(IProvideClassInfo2 *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IProvideClassInfo2(iface);
    return IHTMLXMLHttpRequest_AddRef(&This->IHTMLXMLHttpRequest_iface);
}

static ULONG WINAPI ProvideClassInfo_Release(IProvideClassInfo2 *iface)
{
    HTMLXMLHttpRequest *This = impl_from_IProvideClassInfo2(iface);
    return IHTMLXMLHttpRequest_Release(&This->IHTMLXMLHttpRequest_iface);
}

static HRESULT WINAPI ProvideClassInfo_GetClassInfo(IProvideClassInfo2 *iface, ITypeInfo **ppTI)
{
    HTMLXMLHttpRequest *This = impl_from_IProvideClassInfo2(iface);
    TRACE("(%p)->(%p)\n", This, ppTI);
    return get_class_typeinfo(&CLSID_HTMLXMLHttpRequest, ppTI);
}

static HRESULT WINAPI ProvideClassInfo2_GetGUID(IProvideClassInfo2 *iface, DWORD dwGuidKind, GUID *pGUID)
{
    HTMLXMLHttpRequest *This = impl_from_IProvideClassInfo2(iface);
    FIXME("(%p)->(%lu %p)\n", This, dwGuidKind, pGUID);
    return E_NOTIMPL;
}

static const IProvideClassInfo2Vtbl ProvideClassInfo2Vtbl = {
    ProvideClassInfo_QueryInterface,
    ProvideClassInfo_AddRef,
    ProvideClassInfo_Release,
    ProvideClassInfo_GetClassInfo,
    ProvideClassInfo2_GetGUID,
};

static inline HTMLXMLHttpRequest *impl_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLXMLHttpRequest, event_target.dispex);
}

static nsISupports *HTMLXMLHttpRequest_get_gecko_target(DispatchEx *dispex)
{
    HTMLXMLHttpRequest *This = impl_from_DispatchEx(dispex);
    return (nsISupports*)This->nsxhr;
}

static void HTMLXMLHttpRequest_bind_event(DispatchEx *dispex, eventid_t eid)
{
    HTMLXMLHttpRequest *This = impl_from_DispatchEx(dispex);
    nsIDOMEventTarget *nstarget;
    nsAString type_str;
    const WCHAR *name;
    nsresult nsres;
    unsigned i;

    TRACE("(%p)\n", This);

    for(i = 0; i < ARRAY_SIZE(events); i++)
        if(eid == events[i])
            break;
    if(i >= ARRAY_SIZE(events))
        return;

    if(!This->event_listener) {
        This->event_listener = malloc(sizeof(*This->event_listener));
        if(!This->event_listener)
            return;

        This->event_listener->nsIDOMEventListener_iface.lpVtbl = &XMLHttpReqEventListenerVtbl;
        This->event_listener->ref = 1;
        This->event_listener->event_target = &This->event_target;
        This->event_listener->window = This->window;
        This->event_listener->nsxhr = This->nsxhr;
        This->event_listener->events_mask = 0;
    }

    nsres = nsIXMLHttpRequest_QueryInterface(This->nsxhr, &IID_nsIDOMEventTarget, (void**)&nstarget);
    assert(nsres == NS_OK);

    name = get_event_name(events[i]);
    nsAString_InitDepend(&type_str, name);
    nsres = nsIDOMEventTarget_AddEventListener(nstarget, &type_str, &This->event_listener->nsIDOMEventListener_iface, FALSE, TRUE, 2);
    nsAString_Finish(&type_str);
    if(NS_FAILED(nsres))
        ERR("AddEventListener(%s) failed: %08lx\n", debugstr_w(name), nsres);

    nsIDOMEventTarget_Release(nstarget);

    This->event_listener->events_mask |= 1 << i;
}

static void HTMLXMLHttpRequest_init_dispex_info(dispex_data_t *info, compat_mode_t compat_mode)
{
    static const dispex_hook_t xhr_hooks[] = {
        {DISPID_IHTMLXMLHTTPREQUEST_OPEN, HTMLXMLHttpRequest_open_hook},
        {DISPID_UNKNOWN}
    };
    static const dispex_hook_t private_hooks[] = {
        {DISPID_IWINEXMLHTTPREQUESTPRIVATE_RESPONSE},
        {DISPID_IWINEXMLHTTPREQUESTPRIVATE_RESPONSETYPE},
        {DISPID_IWINEXMLHTTPREQUESTPRIVATE_UPLOAD},
        {DISPID_IWINEXMLHTTPREQUESTPRIVATE_WITHCREDENTIALS},
        {DISPID_EVPROP_ONERROR},
        {DISPID_EVPROP_ONABORT},
        {DISPID_EVPROP_PROGRESS},
        {DISPID_EVPROP_LOADSTART},
        {DISPID_EVPROP_LOADEND},

        /* IE10 only */
        {DISPID_IWINEXMLHTTPREQUESTPRIVATE_OVERRIDEMIMETYPE},
        {DISPID_UNKNOWN}
    };
    const dispex_hook_t *const private_ie10_hooks = private_hooks + ARRAY_SIZE(private_hooks) - 2;

    EventTarget_init_dispex_info(info, compat_mode);
    dispex_info_add_interface(info, IHTMLXMLHttpRequest_tid, compat_mode >= COMPAT_MODE_IE10 ? xhr_hooks : NULL);
    dispex_info_add_interface(info, IWineXMLHttpRequestPrivate_tid,
        compat_mode < COMPAT_MODE_IE10 ? private_hooks :
        compat_mode < COMPAT_MODE_IE11 ? private_ie10_hooks : NULL);
}

static event_target_vtbl_t HTMLXMLHttpRequest_event_target_vtbl = {
    {
        NULL,
    },
    NULL,
    HTMLXMLHttpRequest_get_gecko_target,
    HTMLXMLHttpRequest_bind_event
};

static const tid_t HTMLXMLHttpRequest_iface_tids[] = {
    IHTMLXMLHttpRequest2_tid,
    0
};
dispex_static_data_t HTMLXMLHttpRequest_dispex = {
    L"XMLHttpRequest",
    &HTMLXMLHttpRequest_event_target_vtbl.dispex_vtbl,
    PROTO_ID_HTMLXMLHttpRequest,
    DispHTMLXMLHttpRequest_tid,
    HTMLXMLHttpRequest_iface_tids,
    HTMLXMLHttpRequest_init_dispex_info
};


/* IHTMLXMLHttpRequestFactory */
static inline struct legacy_ctor *impl_from_IHTMLXMLHttpRequestFactory(IHTMLXMLHttpRequestFactory *iface)
{
    return CONTAINING_RECORD(iface, struct legacy_ctor, IHTMLXMLHttpRequestFactory_iface);
}

static HRESULT WINAPI HTMLXMLHttpRequestFactory_QueryInterface(IHTMLXMLHttpRequestFactory *iface, REFIID riid, void **ppv)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        *ppv = &This->IHTMLXMLHttpRequestFactory_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        *ppv = &This->IHTMLXMLHttpRequestFactory_iface;
    }else if(IsEqualGUID(&IID_IHTMLXMLHttpRequestFactory, riid)) {
        *ppv = &This->IHTMLXMLHttpRequestFactory_iface;
    }else if(dispex_query_interface(&This->dispex, riid, ppv)) {
        return *ppv ? S_OK : E_NOINTERFACE;
    }else {
        *ppv = NULL;
        WARN("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI HTMLXMLHttpRequestFactory_AddRef(IHTMLXMLHttpRequestFactory *iface)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLXMLHttpRequestFactory_Release(IHTMLXMLHttpRequestFactory *iface)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        /* Proxy constructor disps hold ref to window, others are always detached first */
        if(This->window)
            IHTMLWindow2_Release(&This->window->base.IHTMLWindow2_iface);
        release_dispex(&This->dispex);
        free(This);
    }

    return ref;
}

static HRESULT WINAPI HTMLXMLHttpRequestFactory_GetTypeInfoCount(IHTMLXMLHttpRequestFactory *iface, UINT *pctinfo)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLXMLHttpRequestFactory_GetTypeInfo(IHTMLXMLHttpRequestFactory *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);

    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLXMLHttpRequestFactory_GetIDsOfNames(IHTMLXMLHttpRequestFactory *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);

    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLXMLHttpRequestFactory_Invoke(IHTMLXMLHttpRequestFactory *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);

    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLXMLHttpRequestFactory_create(IHTMLXMLHttpRequestFactory *iface, IHTMLXMLHttpRequest **p)
{
    struct legacy_ctor *This = impl_from_IHTMLXMLHttpRequestFactory(iface);
    HTMLXMLHttpRequest        *ret;
    nsIXMLHttpRequest         *nsxhr;

    TRACE("(%p)->(%p)\n", This, p);

    nsxhr = create_nsxhr(This->window->base.outer_window->nswindow);
    if(!nsxhr)
        return E_FAIL;

    ret = calloc(1, sizeof(*ret));
    if(!ret) {
        nsIXMLHttpRequest_Release(nsxhr);
        return E_OUTOFMEMORY;
    }
    ret->doctype_override = DOCTYPE_INVALID;
    ret->nsxhr = nsxhr;
    ret->window = This->window;
    IHTMLWindow2_AddRef(&This->window->base.IHTMLWindow2_iface);

    ret->IHTMLXMLHttpRequest_iface.lpVtbl = &HTMLXMLHttpRequestVtbl;
    ret->IHTMLXMLHttpRequest2_iface.lpVtbl = &HTMLXMLHttpRequest2Vtbl;
    ret->IWineXMLHttpRequestPrivate_iface.lpVtbl = &WineXMLHttpRequestPrivateVtbl;
    ret->IProvideClassInfo2_iface.lpVtbl = &ProvideClassInfo2Vtbl;
    ret->ref = 1;
    EventTarget_Init(&ret->event_target, (IUnknown*)&ret->IHTMLXMLHttpRequest_iface,
                     &HTMLXMLHttpRequest_dispex, This->window);

    *p = &ret->IHTMLXMLHttpRequest_iface;
    return S_OK;
}

const IHTMLXMLHttpRequestFactoryVtbl HTMLXMLHttpRequestFactoryVtbl = {
    HTMLXMLHttpRequestFactory_QueryInterface,
    HTMLXMLHttpRequestFactory_AddRef,
    HTMLXMLHttpRequestFactory_Release,
    HTMLXMLHttpRequestFactory_GetTypeInfoCount,
    HTMLXMLHttpRequestFactory_GetTypeInfo,
    HTMLXMLHttpRequestFactory_GetIDsOfNames,
    HTMLXMLHttpRequestFactory_Invoke,
    HTMLXMLHttpRequestFactory_create
};

static inline struct legacy_ctor *ctor_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, struct legacy_ctor, dispex);
}

static HRESULT HTMLXMLHttpRequestFactory_value(DispatchEx *iface, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    struct legacy_ctor *This = ctor_from_DispatchEx(iface);
    IHTMLXMLHttpRequest *xhr;
    HRESULT hres;

    TRACE("\n");

    if(flags != DISPATCH_CONSTRUCT)
        return S_FALSE;

    hres = IHTMLXMLHttpRequestFactory_create(&This->IHTMLXMLHttpRequestFactory_iface, &xhr);
    if(FAILED(hres))
        return hres;

    V_VT(res) = VT_DISPATCH;
    V_DISPATCH(res) = (IDispatch*)xhr;
    return S_OK;
}

static const dispex_static_data_vtbl_t HTMLXMLHttpRequestFactory_dispex_vtbl = {
    HTMLXMLHttpRequestFactory_value,
    legacy_ctor_get_dispid,
    legacy_ctor_get_name,
    legacy_ctor_invoke,
    legacy_ctor_delete
};

static const tid_t HTMLXMLHttpRequestFactory_iface_tids[] = {
    IHTMLXMLHttpRequestFactory_tid,
    0
};
dispex_static_data_t HTMLXMLHttpRequestFactory_dispex = {
    L"XMLHttpRequest",
    &HTMLXMLHttpRequestFactory_dispex_vtbl,
    PROTO_ID_NULL,
    IHTMLXMLHttpRequestFactory_tid,
    HTMLXMLHttpRequestFactory_iface_tids
};

static HRESULT HTMLXMLHttpRequestCtor_value(DispatchEx *iface, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    if(flags == DISPATCH_CONSTRUCT)
        return HTMLXMLHttpRequestFactory_value(iface, lcid, flags, params, res, ei, caller);

    return legacy_ctor_value(iface, lcid, flags, params, res, ei, caller);
}

static const dispex_static_data_vtbl_t HTMLXMLHttpRequestCtor_dispex_vtbl = {
    HTMLXMLHttpRequestCtor_value,
    legacy_ctor_get_dispid,
    legacy_ctor_get_name,
    legacy_ctor_invoke,
    legacy_ctor_delete
};

dispex_static_data_t HTMLXMLHttpRequestCtor_dispex = {
    L"XMLHttpRequest",
    &HTMLXMLHttpRequestCtor_dispex_vtbl,
    PROTO_ID_NULL,
    IHTMLXMLHttpRequestFactory_tid,
    HTMLXMLHttpRequestFactory_iface_tids
};


/* IHTMLXDomainRequest */
static inline HTMLXDomainRequest *impl_from_IHTMLXDomainRequest(IHTMLXDomainRequest *iface)
{
    return CONTAINING_RECORD(iface, HTMLXDomainRequest, IHTMLXDomainRequest_iface);
}

static HRESULT WINAPI HTMLXDomainRequest_QueryInterface(IHTMLXDomainRequest *iface, REFIID riid, void **ppv)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        *ppv = &This->IHTMLXDomainRequest_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        *ppv = &This->IHTMLXDomainRequest_iface;
    }else if(IsEqualGUID(&IID_IHTMLXDomainRequest, riid)) {
        *ppv = &This->IHTMLXDomainRequest_iface;
    }else if(dispex_query_interface(&This->event_target.dispex, riid, ppv)) {
        return *ppv ? S_OK : E_NOINTERFACE;
    }else {
        *ppv = NULL;
        WARN("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI HTMLXDomainRequest_AddRef(IHTMLXDomainRequest *iface)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLXDomainRequest_Release(IHTMLXDomainRequest *iface)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        if(This->event_listener)
            detach_xhr_event_listener(This->event_listener);
        IHTMLWindow2_Release(&This->window->base.IHTMLWindow2_iface);
        release_event_target(&This->event_target);
        release_dispex(&This->event_target.dispex);
        nsIXMLHttpRequest_Release(This->nsxhr);
        free(This);
    }

    return ref;
}

static HRESULT WINAPI HTMLXDomainRequest_GetTypeInfoCount(IHTMLXDomainRequest *iface, UINT *pctinfo)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLXDomainRequest_GetTypeInfo(IHTMLXDomainRequest *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLXDomainRequest_GetIDsOfNames(IHTMLXDomainRequest *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLXDomainRequest_Invoke(IHTMLXDomainRequest *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLXDomainRequest_get_responseText(IHTMLXDomainRequest *iface, BSTR *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    nsAString_InitDepend(&nsstr, NULL);
    nsres = nsIXMLHttpRequest_GetResponseText(This->nsxhr, &nsstr);
    return return_nsstr(nsres, &nsstr, p);
}

static HRESULT WINAPI HTMLXDomainRequest_put_timeout(IHTMLXDomainRequest *iface, LONG v)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%ld)\n", This, v);

    if(v < 0)
        return E_INVALIDARG;
    return map_nsresult(nsIXMLHttpRequest_SetTimeout(This->nsxhr, v));
}

static HRESULT WINAPI HTMLXDomainRequest_get_timeout(IHTMLXDomainRequest *iface, LONG *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    nsresult nsres;
    UINT32 timeout;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_INVALIDARG;

    nsres = nsIXMLHttpRequest_GetTimeout(This->nsxhr, &timeout);
    *p = timeout ? timeout : -1;
    return map_nsresult(nsres);
}

static HRESULT WINAPI HTMLXDomainRequest_get_contentType(IHTMLXDomainRequest *iface, BSTR *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    nsAString nsstr;
    nsresult nsres;
    HRESULT hres;
    UINT16 state;

    TRACE("(%p)->(%p)\n", This, p);

    if(!p)
        return E_POINTER;

    nsres = nsIXMLHttpRequest_GetReadyState(This->nsxhr, &state);
    if(NS_FAILED(nsres) || state < 2) {
        *p = NULL;
        return S_OK;
    }

    nsAString_InitDepend(&nsstr, NULL);
    nsres = nsIXMLHttpRequest_GetResponseText(This->nsxhr, &nsstr);
    if(NS_SUCCEEDED(nsres)) {
        const PRUnichar *data;
        char text[256 * 3];
        WCHAR *mime;
        size_t len;

        nsAString_GetData(&nsstr, &data);
        len = wcslen(data);
        len = WideCharToMultiByte(CP_ACP, 0, data, min(len, 256), text, ARRAY_SIZE(text), NULL, NULL);
        nsAString_Finish(&nsstr);

        if(len) {
            hres = FindMimeFromData(NULL, NULL, text, len, NULL, 0, &mime, 0);
            if(SUCCEEDED(hres)) {
                *p = SysAllocString(mime);
                CoTaskMemFree(mime);
                return *p ? S_OK : E_OUTOFMEMORY;
            }
        }
    }

    *p = SysAllocString(L"text/plain");
    return *p ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI HTMLXDomainRequest_put_onprogress(IHTMLXDomainRequest *iface, VARIANT v)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_PROGRESS, &v);
}

static HRESULT WINAPI HTMLXDomainRequest_get_onprogress(IHTMLXDomainRequest *iface, VARIANT *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_PROGRESS, p);
}

static HRESULT WINAPI HTMLXDomainRequest_put_onerror(IHTMLXDomainRequest *iface, VARIANT v)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_ERROR, &v);
}

static HRESULT WINAPI HTMLXDomainRequest_get_onerror(IHTMLXDomainRequest *iface, VARIANT *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_ERROR, p);
}

static HRESULT WINAPI HTMLXDomainRequest_put_ontimeout(IHTMLXDomainRequest *iface, VARIANT v)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_TIMEOUT, &v);
}

static HRESULT WINAPI HTMLXDomainRequest_get_ontimeout(IHTMLXDomainRequest *iface, VARIANT *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_TIMEOUT, p);
}

static HRESULT WINAPI HTMLXDomainRequest_put_onload(IHTMLXDomainRequest *iface, VARIANT v)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_event_handler(&This->event_target, EVENTID_LOAD, &v);
}

static HRESULT WINAPI HTMLXDomainRequest_get_onload(IHTMLXDomainRequest *iface, VARIANT *p)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_event_handler(&This->event_target, EVENTID_LOAD, p);
}

static HRESULT WINAPI HTMLXDomainRequest_abort(IHTMLXDomainRequest *iface)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    nsresult nsres;

    TRACE("(%p)->()\n", This);

    nsres = nsIXMLHttpRequest_SlowAbort(This->nsxhr);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_SlowAbort failed: %08lx\n", nsres);
        return map_nsresult(nsres);
    }

    return S_OK;
}

static HRESULT WINAPI HTMLXDomainRequest_open(IHTMLXDomainRequest *iface, BSTR bstrMethod, BSTR bstrUrl)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);
    nsAString nsstr, password;
    nsACString str1, str2;
    nsresult nsres;
    HRESULT hres;
    WCHAR *p;

    TRACE("(%p)->(%s %s)\n", This, debugstr_w(bstrMethod), debugstr_w(bstrUrl));

    if((p = wcschr(bstrUrl, ':')) && p[1] == '/' && p[2] == '/') {
        size_t len = p - bstrUrl;
        BSTR bstr;

        /* Native only allows http and https, and the scheme must match */
        if(len < 4 || len > 5 || wcsnicmp(bstrUrl, L"https", len) || !This->window->base.outer_window->uri)
            return E_ACCESSDENIED;

        hres = IUri_GetSchemeName(This->window->base.outer_window->uri, &bstr);
        if(FAILED(hres))
            return hres;
        if(SysStringLen(bstr) != len || wcsnicmp(bstr, bstrUrl, len))
            hres = E_ACCESSDENIED;
        SysFreeString(bstr);
        if(FAILED(hres))
            return hres;
    }

    hres = bstr_to_nsacstr(bstrMethod, &str1);
    if(FAILED(hres))
        return hres;

    hres = bstr_to_nsacstr(bstrUrl, &str2);
    if(FAILED(hres)) {
        nsACString_Finish(&str1);
        return hres;
    }

    nsAString_Init(&nsstr, NULL);
    nsAString_Init(&password, NULL);
    nsres = nsIXMLHttpRequest_Open(This->nsxhr, &str1, &str2, TRUE, &nsstr, &password, 0);
    nsAString_Finish(&nsstr);
    nsAString_Finish(&password);
    nsACString_Finish(&str1);
    nsACString_Finish(&str2);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_Open failed: %08lx\n", nsres);
        return map_nsresult(nsres);
    }

    /* Prevent Gecko from parsing responseXML for no reason */
    nsAString_InitDepend(&nsstr, L"text/plain");
    nsIXMLHttpRequest_SlowOverrideMimeType(This->nsxhr, &nsstr);
    nsAString_Finish(&nsstr);

    /* XDomainRequest only accepts text/plain */
    nsACString_InitDepend(&str1, "Accept");
    nsACString_InitDepend(&str2, "text/plain");
    nsres = nsIXMLHttpRequest_SetRequestHeader(This->nsxhr, &str1, &str2);
    nsACString_Finish(&str1);
    nsACString_Finish(&str2);
    if(NS_FAILED(nsres)) {
        ERR("nsIXMLHttpRequest_SetRequestHeader failed: %08lx\n", nsres);
        return map_nsresult(nsres);
    }

    /* IE always adds Origin header, even from same origin, but Gecko doesn't allow us to alter it. */
    return S_OK;
}

static HRESULT WINAPI HTMLXDomainRequest_send(IHTMLXDomainRequest *iface, VARIANT varBody)
{
    HTMLXDomainRequest *This = impl_from_IHTMLXDomainRequest(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&varBody));

    return nsxhr_send(This->nsxhr, varBody);
}

static const IHTMLXDomainRequestVtbl HTMLXDomainRequestVtbl = {
    HTMLXDomainRequest_QueryInterface,
    HTMLXDomainRequest_AddRef,
    HTMLXDomainRequest_Release,
    HTMLXDomainRequest_GetTypeInfoCount,
    HTMLXDomainRequest_GetTypeInfo,
    HTMLXDomainRequest_GetIDsOfNames,
    HTMLXDomainRequest_Invoke,
    HTMLXDomainRequest_get_responseText,
    HTMLXDomainRequest_put_timeout,
    HTMLXDomainRequest_get_timeout,
    HTMLXDomainRequest_get_contentType,
    HTMLXDomainRequest_put_onprogress,
    HTMLXDomainRequest_get_onprogress,
    HTMLXDomainRequest_put_onerror,
    HTMLXDomainRequest_get_onerror,
    HTMLXDomainRequest_put_ontimeout,
    HTMLXDomainRequest_get_ontimeout,
    HTMLXDomainRequest_put_onload,
    HTMLXDomainRequest_get_onload,
    HTMLXDomainRequest_abort,
    HTMLXDomainRequest_open,
    HTMLXDomainRequest_send
};

static inline HTMLXDomainRequest *XDomainRequest_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLXDomainRequest, event_target.dispex);
}

static nsISupports *HTMLXDomainRequest_get_gecko_target(DispatchEx *dispex)
{
    HTMLXDomainRequest *This = XDomainRequest_from_DispatchEx(dispex);
    return (nsISupports*)This->nsxhr;
}

static void HTMLXDomainRequest_bind_event(DispatchEx *dispex, eventid_t eid)
{
    HTMLXDomainRequest *This = XDomainRequest_from_DispatchEx(dispex);
    nsIDOMEventTarget *nstarget;
    nsAString type_str;
    const WCHAR *name;
    nsresult nsres;
    unsigned i;

    TRACE("(%p)\n", This);

    for(i = 0; i < ARRAY_SIZE(events); i++)
        if(eid == events[i])
            break;
    if(i >= ARRAY_SIZE(events))
        return;

    if(!This->event_listener) {
        This->event_listener = malloc(sizeof(*This->event_listener));
        if(!This->event_listener)
            return;

        This->event_listener->nsIDOMEventListener_iface.lpVtbl = &XMLHttpReqEventListenerVtbl;
        This->event_listener->ref = 1;
        This->event_listener->event_target = &This->event_target;
        This->event_listener->window = This->window;
        This->event_listener->nsxhr = This->nsxhr;
        This->event_listener->events_mask = 0;
    }

    nsres = nsIXMLHttpRequest_QueryInterface(This->nsxhr, &IID_nsIDOMEventTarget, (void**)&nstarget);
    assert(nsres == NS_OK);

    name = get_event_name(events[i]);
    nsAString_InitDepend(&type_str, name);
    nsres = nsIDOMEventTarget_AddEventListener(nstarget, &type_str, &This->event_listener->nsIDOMEventListener_iface, FALSE, TRUE, 2);
    nsAString_Finish(&type_str);
    if(NS_FAILED(nsres))
        ERR("AddEventListener(%s) failed: %08lx\n", debugstr_w(name), nsres);

    nsIDOMEventTarget_Release(nstarget);

    This->event_listener->events_mask |= 1 << i;
}

static void HTMLXDomainRequest_init_dispex_info(dispex_data_t *info, compat_mode_t compat_mode)
{
    dispex_info_add_interface(info, IHTMLXDomainRequest_tid, NULL);
}

static event_target_vtbl_t HTMLXDomainRequest_event_target_vtbl = {
    {
        NULL
    },
    NULL,
    HTMLXDomainRequest_get_gecko_target,
    HTMLXDomainRequest_bind_event
};

dispex_static_data_t HTMLXDomainRequest_dispex = {
    L"XDomainRequest",
    &HTMLXDomainRequest_event_target_vtbl.dispex_vtbl,
    PROTO_ID_HTMLXDomainRequest,
    DispXDomainRequest_tid,
    no_iface_tids,
    HTMLXDomainRequest_init_dispex_info
};


/* IHTMLXDomainRequestFactory */
static inline struct legacy_ctor *impl_from_IHTMLXDomainRequestFactory(IHTMLXDomainRequestFactory *iface)
{
    return CONTAINING_RECORD(iface, struct legacy_ctor, IHTMLXDomainRequestFactory_iface);
}

static HRESULT WINAPI HTMLXDomainRequestFactory_QueryInterface(IHTMLXDomainRequestFactory *iface, REFIID riid, void **ppv)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        *ppv = &This->IHTMLXDomainRequestFactory_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid) || IsEqualGUID(&IID_IHTMLXDomainRequestFactory, riid)) {
        *ppv = &This->IHTMLXDomainRequestFactory_iface;
    }else if(dispex_query_interface(&This->dispex, riid, ppv)) {
        return *ppv ? S_OK : E_NOINTERFACE;
    }else {
        *ppv = NULL;
        WARN("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI HTMLXDomainRequestFactory_AddRef(IHTMLXDomainRequestFactory *iface)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLXDomainRequestFactory_Release(IHTMLXDomainRequestFactory *iface)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        /* Proxy constructor disps hold ref to window, others are always detached first */
        if(This->window)
            IHTMLWindow2_Release(&This->window->base.IHTMLWindow2_iface);
        release_dispex(&This->dispex);
        free(This);
    }

    return ref;
}

static HRESULT WINAPI HTMLXDomainRequestFactory_GetTypeInfoCount(IHTMLXDomainRequestFactory *iface, UINT *pctinfo)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLXDomainRequestFactory_GetTypeInfo(IHTMLXDomainRequestFactory *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLXDomainRequestFactory_GetIDsOfNames(IHTMLXDomainRequestFactory *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLXDomainRequestFactory_Invoke(IHTMLXDomainRequestFactory *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLXDomainRequestFactory_create(IHTMLXDomainRequestFactory *iface, IHTMLXDomainRequest **p)
{
    struct legacy_ctor *This = impl_from_IHTMLXDomainRequestFactory(iface);
    nsIXMLHttpRequest *nsxhr;
    HTMLXDomainRequest *ret;

    TRACE("(%p)->(%p)\n", This, p);

    nsxhr = create_nsxhr(This->window->base.outer_window->nswindow);
    if(!nsxhr)
        return E_FAIL;

    ret = calloc(1, sizeof(*ret));
    if(!ret) {
        nsIXMLHttpRequest_Release(nsxhr);
        return E_OUTOFMEMORY;
    }
    ret->nsxhr = nsxhr;
    ret->window = This->window;
    IHTMLWindow2_AddRef(&This->window->base.IHTMLWindow2_iface);

    ret->IHTMLXDomainRequest_iface.lpVtbl = &HTMLXDomainRequestVtbl;
    ret->ref = 1;
    EventTarget_Init(&ret->event_target, (IUnknown*)&ret->IHTMLXDomainRequest_iface,
                     &HTMLXDomainRequest_dispex, This->window);

    *p = &ret->IHTMLXDomainRequest_iface;
    return S_OK;
}

const IHTMLXDomainRequestFactoryVtbl HTMLXDomainRequestFactoryVtbl = {
    HTMLXDomainRequestFactory_QueryInterface,
    HTMLXDomainRequestFactory_AddRef,
    HTMLXDomainRequestFactory_Release,
    HTMLXDomainRequestFactory_GetTypeInfoCount,
    HTMLXDomainRequestFactory_GetTypeInfo,
    HTMLXDomainRequestFactory_GetIDsOfNames,
    HTMLXDomainRequestFactory_Invoke,
    HTMLXDomainRequestFactory_create
};

static HRESULT HTMLXDomainRequestFactory_value(DispatchEx *iface, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    struct legacy_ctor *This = ctor_from_DispatchEx(iface);
    IHTMLXDomainRequest *xdr;
    HRESULT hres;

    TRACE("\n");

    if(flags != DISPATCH_CONSTRUCT)
        return S_FALSE;

    hres = IHTMLXDomainRequestFactory_create(&This->IHTMLXDomainRequestFactory_iface, &xdr);
    if(FAILED(hres))
        return hres;

    V_VT(res) = VT_DISPATCH;
    V_DISPATCH(res) = (IDispatch*)xdr;
    return S_OK;
}

static const dispex_static_data_vtbl_t HTMLXDomainRequestFactory_dispex_vtbl = {
    HTMLXDomainRequestFactory_value,
    legacy_ctor_get_dispid,
    legacy_ctor_get_name,
    legacy_ctor_invoke,
    legacy_ctor_delete
};

static const tid_t HTMLXDomainRequestFactory_iface_tids[] = {
    IHTMLXDomainRequestFactory_tid,
    0
};
dispex_static_data_t HTMLXDomainRequestFactory_dispex = {
    L"XDomainRequest",
    &HTMLXDomainRequestFactory_dispex_vtbl,
    PROTO_ID_NULL,
    IHTMLXDomainRequestFactory_tid,
    HTMLXDomainRequestFactory_iface_tids
};

static HRESULT HTMLXDomainRequestCtor_value(DispatchEx *iface, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    if(flags == DISPATCH_CONSTRUCT)
        return HTMLXDomainRequestFactory_value(iface, lcid, flags, params, res, ei, caller);

    return legacy_ctor_value(iface, lcid, flags, params, res, ei, caller);
}

static const dispex_static_data_vtbl_t HTMLXDomainRequestCtor_dispex_vtbl = {
    HTMLXDomainRequestCtor_value,
    legacy_ctor_get_dispid,
    legacy_ctor_get_name,
    legacy_ctor_invoke,
    legacy_ctor_delete
};

dispex_static_data_t HTMLXDomainRequestCtor_dispex = {
    L"XDomainRequest",
    &HTMLXDomainRequestCtor_dispex_vtbl,
    PROTO_ID_NULL,
    IHTMLXDomainRequestFactory_tid,
    HTMLXDomainRequestFactory_iface_tids
};
