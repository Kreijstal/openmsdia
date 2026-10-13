/* typelib_dump: write the type library embedded in a COM DLL as JSON.
 *
 * Usage: typelib_dump <file.dll> > out.json
 *
 * Output is deterministic: types in library order, functions in vtable
 * order (oVft), no timestamps or paths. Only interface facts (names,
 * GUIDs, signatures, enum values) are emitted, never code.
 *
 * Build (MSYS2 CLANG64/UCRT64): cc -O2 -o typelib_dump typelib_dump.c -loleaut32 -lole32 -luuid
 */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdlib.h>

/* oleaut32 lays the vtable out for the *loading* process, whatever the
 * library's syskind, so slots are oVft / sizeof(void *) of this tool. */
static const int ptr_size = sizeof(void *);

static void put_str(const WCHAR *s)
{
    putchar('"');
    if (s)
        for (; *s; s++) {
            unsigned c = *s;
            if (c == '"' || c == '\\') printf("\\%c", c);
            else if (c < 0x20 || c > 0x7e) printf("\\u%04x", c);
            else putchar(c);
        }
    putchar('"');
}

static void put_guid(const GUID *g)
{
    printf("\"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X\"",
           (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
           g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static const char *vt_name(VARTYPE vt)
{
    switch (vt) {
    case VT_EMPTY: return "void*empty"; case VT_I2: return "short"; case VT_I4: return "long";
    case VT_R4: return "float"; case VT_R8: return "double"; case VT_CY: return "CY";
    case VT_DATE: return "DATE"; case VT_BSTR: return "BSTR"; case VT_DISPATCH: return "IDispatch*";
    case VT_ERROR: return "SCODE"; case VT_BOOL: return "VARIANT_BOOL"; case VT_VARIANT: return "VARIANT";
    case VT_UNKNOWN: return "IUnknown*"; case VT_DECIMAL: return "DECIMAL"; case VT_I1: return "char";
    case VT_UI1: return "BYTE"; case VT_UI2: return "USHORT"; case VT_UI4: return "ULONG";
    case VT_I8: return "LONGLONG"; case VT_UI8: return "ULONGLONG"; case VT_INT: return "int";
    case VT_UINT: return "UINT"; case VT_VOID: return "void"; case VT_HRESULT: return "HRESULT";
    case VT_LPSTR: return "LPSTR"; case VT_LPWSTR: return "LPWSTR";
    default: return NULL;
    }
}

/* Type descriptions as a small JSON tree: {"ptr":…}, {"carray":…,"dims":[…]},
 * {"ref":"Name"}, or {"vt":"long"}. */
static void put_type(ITypeInfo *ti, const TYPEDESC *td)
{
    switch (td->vt) {
    case VT_PTR:
        printf("{\"ptr\":"); put_type(ti, td->lptdesc); putchar('}');
        return;
    case VT_SAFEARRAY:
        printf("{\"safearray\":"); put_type(ti, td->lptdesc); putchar('}');
        return;
    case VT_CARRAY: {
        const ARRAYDESC *ad = td->lpadesc;
        printf("{\"carray\":"); put_type(ti, &ad->tdescElem); printf(",\"dims\":[");
        for (USHORT i = 0; i < ad->cDims; i++)
            printf("%s%lu", i ? "," : "", (unsigned long)ad->rgbounds[i].cElements);
        printf("]}");
        return;
    }
    case VT_USERDEFINED: {
        ITypeInfo *ref = NULL; BSTR name = NULL;
        if (SUCCEEDED(ITypeInfo_GetRefTypeInfo(ti, td->hreftype, &ref)) &&
            SUCCEEDED(ITypeInfo_GetDocumentation(ref, MEMBERID_NIL, &name, NULL, NULL, NULL))) {
            printf("{\"ref\":"); put_str(name); putchar('}');
        } else
            printf("{\"ref\":null}");
        SysFreeString(name);
        if (ref) ITypeInfo_Release(ref);
        return;
    }
    default: {
        const char *n = vt_name(td->vt);
        if (n) printf("{\"vt\":\"%s\"}", n);
        else printf("{\"vt\":%u}", td->vt);
    }
    }
}

static const char *tkind_name(TYPEKIND k)
{
    static const char *n[] = {"enum", "record", "module", "interface", "dispatch", "coclass", "alias", "union"};
    return k < TKIND_MAX ? n[k] : "unknown";
}

static const char *invkind_name(INVOKEKIND k)
{
    switch (k) {
    case INVOKE_FUNC: return "func"; case INVOKE_PROPERTYGET: return "propget";
    case INVOKE_PROPERTYPUT: return "propput"; case INVOKE_PROPERTYPUTREF: return "propputref";
    default: return "unknown";
    }
}

static void put_flags_param(USHORT f)
{
    printf("[");
    int first = 1;
#define F(bit, s) if (f & bit) { printf("%s\"%s\"", first ? "" : ",", s); first = 0; }
    F(PARAMFLAG_FIN, "in") F(PARAMFLAG_FOUT, "out") F(PARAMFLAG_FLCID, "lcid")
    F(PARAMFLAG_FRETVAL, "retval") F(PARAMFLAG_FOPT, "optional") F(PARAMFLAG_FHASDEFAULT, "default")
#undef F
    printf("]");
}

static void dump_funcs(ITypeInfo *ti, const TYPEATTR *ta)
{
    printf(",\"funcs\":[");
    for (UINT i = 0; i < ta->cFuncs; i++) {
        FUNCDESC *fd;
        if (FAILED(ITypeInfo_GetFuncDesc(ti, i, &fd))) continue;
        BSTR names[64]; UINT n = 0;
        ITypeInfo_GetNames(ti, fd->memid, names, 64, &n);
        printf("%s\n    {\"name\":", i ? "," : "");
        put_str(n ? names[0] : NULL);
        printf(",\"memid\":%ld,\"vtbl_offset\":%d,\"slot\":%d,\"kind\":\"%s\",\"ret\":",
               (long)fd->memid, fd->oVft, fd->oVft / ptr_size,
               invkind_name(fd->invkind));
        put_type(ti, &fd->elemdescFunc.tdesc);
        printf(",\"params\":[");
        for (SHORT p = 0; p < fd->cParams; p++) {
            printf("%s{\"name\":", p ? "," : "");
            /* propput's value parameter has no name in the library. */
            put_str((UINT)(p + 1) < n ? names[p + 1] : NULL);
            printf(",\"type\":");
            put_type(ti, &fd->lprgelemdescParam[p].tdesc);
            printf(",\"flags\":");
            put_flags_param(fd->lprgelemdescParam[p].paramdesc.wParamFlags);
            putchar('}');
        }
        printf("]}");
        for (UINT k = 0; k < n; k++) SysFreeString(names[k]);
        ITypeInfo_ReleaseFuncDesc(ti, fd);
    }
    printf("]");
}

static void dump_vars(ITypeInfo *ti, const TYPEATTR *ta)
{
    printf(",\"members\":[");
    for (UINT i = 0; i < ta->cVars; i++) {
        VARDESC *vd;
        if (FAILED(ITypeInfo_GetVarDesc(ti, i, &vd))) continue;
        BSTR name = NULL; UINT n = 0;
        ITypeInfo_GetNames(ti, vd->memid, &name, 1, &n);
        printf("%s\n    {\"name\":", i ? "," : ""); put_str(name);
        printf(",\"type\":"); put_type(ti, &vd->elemdescVar.tdesc);
        if (vd->varkind == VAR_CONST && vd->lpvarValue) {
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(VariantChangeType(&v, vd->lpvarValue, 0, VT_I8)))
                printf(",\"value\":%lld", (long long)v.llVal);
            VariantClear(&v);
        } else if (vd->varkind == VAR_PERINSTANCE)
            printf(",\"offset\":%lu", (unsigned long)vd->oInst);
        putchar('}');
        SysFreeString(name);
        ITypeInfo_ReleaseVarDesc(ti, vd);
    }
    printf("]");
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s file.dll\n", argv[0]); return 2; }
    WCHAR path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path, MAX_PATH);
    CoInitialize(NULL);
    ITypeLib *tl;
    HRESULT hr = LoadTypeLibEx(path, REGKIND_NONE, &tl);
    if (FAILED(hr)) { fprintf(stderr, "LoadTypeLibEx: 0x%08lx\n", (unsigned long)hr); return 1; }

    TLIBATTR *la; BSTR libname = NULL;
    ITypeLib_GetLibAttr(tl, &la);
    ITypeLib_GetDocumentation(tl, MEMBERID_NIL, &libname, NULL, NULL, NULL);
    printf("{\"library\":"); put_str(libname);
    printf(",\"guid\":"); put_guid(&la->guid);
    printf(",\"version\":\"%u.%u\",\"syskind\":\"%s\",\"types\":[", la->wMajorVerNum, la->wMinorVerNum,
           la->syskind == SYS_WIN64 ? "win64" : la->syskind == SYS_WIN32 ? "win32" : "other");
    ITypeLib_ReleaseTLibAttr(tl, la);
    SysFreeString(libname);

    UINT count = ITypeLib_GetTypeInfoCount(tl);
    for (UINT i = 0; i < count; i++) {
        ITypeInfo *ti; TYPEATTR *ta; BSTR name = NULL;
        if (FAILED(ITypeLib_GetTypeInfo(tl, i, &ti))) continue;
        ITypeInfo_GetTypeAttr(ti, &ta);
        ITypeInfo_GetDocumentation(ti, MEMBERID_NIL, &name, NULL, NULL, NULL);
        printf("%s\n {\"name\":", i ? "," : ""); put_str(name);
        printf(",\"kind\":\"%s\",\"guid\":", tkind_name(ta->typekind)); put_guid(&ta->guid);
        printf(",\"flags\":%u", ta->wTypeFlags);
        if (ta->typekind == TKIND_ALIAS) { printf(",\"alias\":"); put_type(ti, &ta->tdescAlias); }
        if (ta->typekind == TKIND_RECORD || ta->typekind == TKIND_UNION)
            printf(",\"size\":%lu", (unsigned long)ta->cbSizeInstance);
        if (ta->cImplTypes) {
            printf(",\"implements\":[");
            for (UINT k = 0; k < ta->cImplTypes; k++) {
                HREFTYPE h; ITypeInfo *base; BSTR bn = NULL; INT f = 0;
                ITypeInfo_GetImplTypeFlags(ti, k, &f);
                if (SUCCEEDED(ITypeInfo_GetRefTypeOfImplType(ti, k, &h)) &&
                    SUCCEEDED(ITypeInfo_GetRefTypeInfo(ti, h, &base))) {
                    ITypeInfo_GetDocumentation(base, MEMBERID_NIL, &bn, NULL, NULL, NULL);
                    ITypeInfo_Release(base);
                }
                printf("%s{\"name\":", k ? "," : ""); put_str(bn);
                printf(",\"default\":%s}", (f & IMPLTYPEFLAG_FDEFAULT) ? "true" : "false");
                SysFreeString(bn);
            }
            printf("]");
        }
        if (ta->cFuncs) dump_funcs(ti, ta);
        if (ta->cVars) dump_vars(ti, ta);
        printf("}");
        SysFreeString(name);
        ITypeInfo_ReleaseTypeAttr(ti, ta);
        ITypeInfo_Release(ti);
    }
    printf("\n]}\n");
    ITypeLib_Release(tl);
    CoUninitialize();
    return 0;
}
