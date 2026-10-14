// OpenMSDIA name corpus: C++ constructs chosen for their decorated names,
// not for their debug information. Built with MSVC (CI) and clang-cl into
// names PDBs whose public symbols feed the undecoration test
// (walker/name_probe.cpp, tools/undname_test.py). Freestanding; every
// function is referenced from start() or from a global, so it is emitted.

#ifdef _MSC_VER
extern "C" int _fltused = 0;
extern "C" int __cdecl _purecall() { for (;;) {} }
#endif
extern "C" void __cxa_pure_virtual() { for (;;) {} }
extern "C" int __cdecl atexit(void(__cdecl *)(void)) { return 0; }
typedef decltype(sizeof 0) size_t;
extern "C" void *memset(void *d, int c, size_t n)
{
    for (size_t i = 0; i < n; i++) ((volatile char *)d)[i] = (char)c;
    return d;
}
extern "C" void *memcpy(void *d, const void *s, size_t n)
{
    for (size_t i = 0; i < n; i++) ((volatile char *)d)[i] = ((const char *)s)[i];
    return d;
}
typedef decltype(nullptr) nullptr_t;
static char heap[4096];
static size_t heap_used;
void *operator new(size_t n) { heap_used += n; return heap + heap_used - n; }
void *operator new[](size_t n) { heap_used += n; return heap + heap_used - n; }
void operator delete(void *) noexcept {}
void operator delete(void *, size_t) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete[](void *, size_t) noexcept {}
void *operator new(size_t, void *p) noexcept { return p; }

// RTTI descriptors refer to type_info's vftable, which normally comes from
// the C++ runtime.
extern "C" const void *openmsdia_type_info_vft[2] = {0, 0};
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:??_7type_info@@6B@=_openmsdia_type_info_vft")
#else
#pragma comment(linker, "/alternatename:??_7type_info@@6B@=openmsdia_type_info_vft")
#endif

// MSVC rejects __unaligned on x86.
#if defined(_M_IX86) && !defined(__clang__)
#define UNALIGNED
#else
#define UNALIGNED __unaligned
#endif

// ------------------------------------------------------------ basic types
int g_int;
extern const int g_cint = 3;
volatile long g_vlong;
unsigned char g_uchar;
signed char g_schar;
char g_char;
wchar_t g_wchar;
char8_t g_char8;
char16_t g_char16;
char32_t g_char32;
bool g_bool;
short g_short;
unsigned short g_ushort;
long long g_ll;
unsigned long long g_ull;
float g_float;
double g_double;
long double g_ldouble;
int *g_pint;
const int *g_pcint;
extern int *const g_cpint = nullptr;
int *volatile g_vpint;
int **g_ppint;
const char *const *g_ppcc;
int g_arr[3][4];
int (*g_parr)[5];
int (&g_rarr)[3][4] = g_arr;
int &g_ref = g_int;
int *__restrict g_rptr;
UNALIGNED int *g_uptr;
void (*g_fp)(int, ...);
int (*(*g_fpfp)(char))(double);
nullptr_t g_null;

int f_void() { return 0; }
void f_args(char, signed char, unsigned char, short, unsigned short, int, unsigned, long, unsigned long) {}
void f_args2(long long, unsigned long long, float, double, long double, bool, wchar_t) {}
void f_args3(char8_t, char16_t, char32_t, nullptr_t) {}
void f_ptrs(int *, const int *, volatile int *, const volatile int *, int *const, int &, const int &, int &&) {}
void f_arrays(int[3], int (*)[3], int (&)[3], int (&)[2][3]) {}
void vfun(int) {}
void f_funcs(void (*)(), int (*)(int, int), void (&)(int), void (*)(int, ...)) {}
int f_variadic(int, ...) { return 0; }
void f_backrefs(int *, int *, const char *, const char *, double, double) {}
const int *f_retcptr() { return nullptr; }
const int &f_retcref() { return g_int; }
int &&f_retrref() { return static_cast<int &&>(g_int); }
volatile int f_retv() { return 0; }
int (*f_retfp())(int) { return nullptr; }
int (*f_retparr())[3] { return nullptr; }
void f_noexcept() noexcept {}
void f_restrict(int *__restrict, char *__restrict) {}
void f_unaligned(UNALIGNED int *) {}

#if defined(_M_IX86)
int __stdcall f_stdcall(int a, int b) { return a + b; }
int __fastcall f_fastcall(int a, int b) { return a + b; }
#endif
int __vectorcall f_vectorcall(int a, double b) { return a + (int)b; }
int __cdecl f_cdecl(int a) { return a; }

extern "C" int c_function(int a) { return a; }
extern "C" int c_data = 1;
#if defined(_M_IX86)
extern "C" int __stdcall c_stdcall(int a, int b) { return a + b; }
extern "C" int __fastcall c_fastcall(int a, int b) { return a + b; }
#endif
extern "C" int __vectorcall c_vectorcall(int a, int b) { return a + b; }

// ------------------------------------------------------------ enums, UDTs
enum Color { Red, Green };
enum class Small : char { A };
enum class Big : unsigned long long { B };
enum class Neg : short { C = -1 };
struct S { int s; };
union U { int i; float f; };
class K { public: int k; };
void f_udts(Color, Small, Big, Neg, S, U, K, S *, const U &, K &&) {}
Color g_color;
S g_s;
U g_u;
K g_k;

// ------------------------------------------------------------ namespaces
namespace outer {
namespace inner {
int deep(int x) { return x; }
int deep_data;
struct Deep {
    struct Nested {
        static int value;
        int get() const;
    };
};
int Deep::Nested::value = 1;
int Deep::Nested::get() const { return value; }
}  // namespace inner
inline namespace v1 {
int versioned() { return 1; }
}
}  // namespace outer

namespace {
int anon_func(int x) { return x + 1; }
int anon_data;
struct AnonS {
    int get() { return 2; }
};
}  // namespace

int call_anon() { return anon_func(1) + anon_data + AnonS().get(); }

// ------------------------------------------------------------ operators
struct Ops {
    int v;
    Ops() : v(0) {}
    Ops(int x) : v(x) {}
    Ops(const Ops &o) : v(o.v) {}
    Ops(Ops &&o) noexcept : v(o.v) {}
    ~Ops() {}
    Ops &operator=(const Ops &o) { v = o.v; return *this; }
    Ops &operator=(Ops &&o) noexcept { v = o.v; return *this; }
    Ops operator+(const Ops &o) const { return Ops(v + o.v); }
    Ops operator-(const Ops &o) const { return Ops(v - o.v); }
    Ops operator*(const Ops &o) const { return Ops(v * o.v); }
    Ops operator/(const Ops &o) const { return Ops(v / o.v); }
    Ops operator%(const Ops &o) const { return Ops(v % o.v); }
    Ops operator^(const Ops &o) const { return Ops(v ^ o.v); }
    Ops operator&(const Ops &o) const { return Ops(v & o.v); }
    Ops operator|(const Ops &o) const { return Ops(v | o.v); }
    Ops operator~() const { return Ops(~v); }
    bool operator!() const { return !v; }
    bool operator<(const Ops &o) const { return v < o.v; }
    bool operator>(const Ops &o) const { return v > o.v; }
    bool operator<=(const Ops &o) const { return v <= o.v; }
    bool operator>=(const Ops &o) const { return v >= o.v; }
    bool operator==(const Ops &o) const { return v == o.v; }
    bool operator!=(const Ops &o) const { return v != o.v; }
    int operator<=>(const Ops &o) const { return v - o.v; }
    Ops &operator+=(const Ops &o) { v += o.v; return *this; }
    Ops &operator-=(const Ops &o) { v -= o.v; return *this; }
    Ops &operator*=(const Ops &o) { v *= o.v; return *this; }
    Ops &operator/=(const Ops &o) { v /= o.v; return *this; }
    Ops &operator%=(const Ops &o) { v %= o.v; return *this; }
    Ops &operator^=(const Ops &o) { v ^= o.v; return *this; }
    Ops &operator&=(const Ops &o) { v &= o.v; return *this; }
    Ops &operator|=(const Ops &o) { v |= o.v; return *this; }
    Ops &operator<<=(int n) { v <<= n; return *this; }
    Ops &operator>>=(int n) { v >>= n; return *this; }
    Ops operator<<(int n) const { return Ops(v << n); }
    Ops operator>>(int n) const { return Ops(v >> n); }
    bool operator&&(const Ops &o) const { return v && o.v; }
    bool operator||(const Ops &o) const { return v || o.v; }
    Ops &operator++() { ++v; return *this; }
    Ops operator++(int) { return Ops(v++); }
    Ops &operator--() { --v; return *this; }
    Ops operator--(int) { return Ops(v--); }
    Ops operator,(const Ops &o) const { return o; }
    int operator->*(int Ops::*m) const { return this->*m; }
    Ops *operator->() { return this; }
    int operator()(int a, int b) const { return v + a + b; }
    int operator[](int i) const { return v + i; }
    Ops operator-() const { return Ops(-v); }
    Ops operator+() const { return *this; }
    Ops *operator&() { return this; }
    int operator*() const { return v; }
    operator int() const { return v; }
    operator bool() const { return v != 0; }
    explicit operator const char *() const { return nullptr; }
    void *operator new(size_t n) { return ::operator new(n); }
    void operator delete(void *p) { ::operator delete(p); }
    void *operator new[](size_t n) { return ::operator new[](n); }
    void operator delete[](void *p) { ::operator delete[](p); }
};
unsigned long long operator""_km(unsigned long long x) { return x * 1000; }

int use_ops()
{
    Ops a(1), b(2);
    Ops c = a + b - a * b / b % b ^ a & b | ~a;
    c += a; c -= a; c *= b; c /= b; c %= b; c ^= a; c &= b; c |= a; c <<= 1; c >>= 1;
    c = c << 1 >> 1;
    bool r = !a || (a < b && a > b) || a <= b || a >= b || a == b || a != b || (a && b) || (a || b);
    int cmp = a <=> b;
    ++c; c++; --c; c--;
    c = (a, b);
    int Ops::*m = &Ops::v;
    int x = c->*m + c->v + c(1, 2) + c[3] + (-c).v + (+c).v + (*c) + (int)c + (bool)c + ((&c)->v);
    const char *p = (const char *)c;
    Ops *h = new Ops(5);
    delete h;
    Ops moved(static_cast<Ops &&>(c));
    moved = static_cast<Ops &&>(a);
    moved = b;
    return x + r + cmp + (p != nullptr) + (int)(5_km);
}

// ------------------------------------------------------------ classes
class Access {
public:
    int pub(int) { return 1; }
    static int spub;
    virtual int vpub() { return 2; }
    int cpub() const { return 3; }
    int vlpub() volatile { return 4; }
    int cvpub() const volatile { return 5; }
    int lref() & { return 6; }
    int rref() && { return 7; }
    int clref() const & { return 8; }
    static int __cdecl sfunc(int) { return 9; }
protected:
    int prot(int) { return 10; }
    static int sprot;
    virtual int vprot() { return 11; }
private:
    int priv(int) { return 12; }
    static int spriv;
    virtual int vpriv() { return 13; }
public:
    int all() { return prot(0) + priv(0) + vprot() + vpriv() + sprot + spriv; }
    virtual ~Access() {}
};
int Access::spub = 1;
int Access::sprot = 2;
int Access::spriv = 3;

struct Abstract {
    virtual int pure() = 0;
    virtual ~Abstract() {}
};
struct Concrete : Abstract {
    int pure() override { return 1; }
};

// Multiple and virtual inheritance: vftables "for" a base, vbtables,
// adjustor and vtordisp thunks.
struct VA { int a = 1; virtual int fa() { return a; } virtual ~VA() {} };
struct VB { int b = 2; virtual int fb() { return b; } virtual int fboth() { return 1; } virtual ~VB() {} };
struct VC : VA, VB { int c = 3; int fb() override { return c; } int fboth() override { return 2; } };
struct VBase { int vb = 4; virtual int fv() { return vb; } virtual ~VBase() {} };
struct VD1 : virtual VBase { int d1 = 5; int fv() override { return d1; } VD1() {} };
struct VD2 : virtual VBase { int d2 = 6; };
struct VDiamond : VD1, VD2 { int dd = 7; int fv() override { return dd; } VDiamond() {} };
#pragma vtordisp(push, 2)
struct VT : virtual VBase {
    int t = 8;
    VT() { fv(); }
    int fv() override { return t; }
};
#pragma vtordisp(pop)

// ------------------------------------------------------------ member pointers
struct MP {
    int x;
    double y;
    int f(int) { return x; }
    virtual int vf(int) { return x + 1; }
    int cf() const { return x; }
};
struct MPMulti : VA, VB { int m; int g() { return m; } };
struct MPVirt : virtual VBase { int n; int h() { return n; } };
struct MPUnknown;
int MP::*g_mp_data = &MP::x;
double MP::*g_mp_double = &MP::y;
int (MP::*g_mp_func)(int) = &MP::f;
int (MP::*g_mp_vfunc)(int) = &MP::vf;
int (MP::*g_mp_cfunc)() const = &MP::cf;
int (MPMulti::*g_mp_multi)() = &MPMulti::g;
int (MPVirt::*g_mp_virt)() = &MPVirt::h;
int MPMulti::*g_mp_multi_data = &MPMulti::m;
int MPVirt::*g_mp_virt_data = &MPVirt::n;
int MPUnknown::*g_mp_unknown_data;
void (MPUnknown::*g_mp_unknown_func)();
int f_memptrs(int MP::*, int (MP::*)(int), int (MP::*)() const, int MPVirt::*) { return 0; }

// ------------------------------------------------------------ templates
template <class T> struct Box {
    T value;
    T get() const { return value; }
    static T shared;
    template <class U> U as() const { return (U)value; }
    struct Inner { T t; T in() { return t; } };
};
template <class T> T Box<T>::shared;
template struct Box<int>;
template struct Box<const char *>;
template struct Box<Box<int>>;
template struct Box<Box<Box<int>>>;
template struct Box<int (*)(int)>;
template struct Box<int MP::*>;
template struct Box<int (MP::*)(int)>;
template struct Box<nullptr_t>;
template struct Box<S>;
template struct Box<Color>;
template struct Box<Small>;
template double Box<int>::as<double>() const;

template <int N> struct IntT { static int get() { return N; } };
template struct IntT<0>;
template struct IntT<1>;
template struct IntT<-1>;
template struct IntT<10>;
template struct IntT<11>;
template struct IntT<16>;
template struct IntT<255>;
template struct IntT<-256>;
template struct IntT<2147483647>;
template struct IntT<(-2147483647 - 1)>;
template <unsigned long long N> struct ULLT { static unsigned long long get() { return N; } };
template struct ULLT<18446744073709551615ull>;
template <bool B> struct BoolT { static bool get() { return B; } };
template struct BoolT<true>;
template struct BoolT<false>;
template <char C> struct CharT { static char get() { return C; } };
template struct CharT<'a'>;
template <Color C> struct EnumT { static int get() { return C; } };
template struct EnumT<Green>;
template <Small C> struct EnumClassT { static int get() { return (int)C; } };
template struct EnumClassT<Small::A>;

template <int *P> struct PtrT { static int *get() { return P; } };
template struct PtrT<&g_int>;
template struct PtrT<nullptr>;
template <int &R> struct RefT { static int &get() { return R; } };
template struct RefT<g_int>;
template <int (*F)()> struct FuncT { static int get() { return F(); } };
template struct FuncT<&f_void>;
template <int MP::*M> struct MemDataT { static int get(MP &m) { return m.*M; } };
template struct MemDataT<&MP::x>;
template struct MemDataT<nullptr>;
template <int (MP::*M)(int)> struct MemFuncT { static int get(MP &m) { return (m.*M)(1); } };
template struct MemFuncT<&MP::f>;
template struct MemFuncT<&MP::vf>;
template <int (MPMulti::*M)()> struct MemFuncMultiT { static int get(MPMulti &m) { return (m.*M)(); } };
template struct MemFuncMultiT<&MPMulti::g>;
template <int (MPVirt::*M)()> struct MemFuncVirtT { static int get(MPVirt &m) { return (m.*M)(); } };
template struct MemFuncVirtT<&MPVirt::h>;
template <auto V> struct AutoT { static int get() { return (int)V; } };
template struct AutoT<5>;
template struct AutoT<'x'>;
template struct AutoT<true>;
template <template <class> class TT> struct TemplT { static int get() { return sizeof(TT<int>); } };
template struct TemplT<Box>;
template <class... Ts> struct Pack { static int count() { return sizeof...(Ts); } };
template struct Pack<>;
template struct Pack<int>;
template struct Pack<int, char, double>;
template <int... Ns> struct IntPack { static int count() { return sizeof...(Ns); } };
template struct IntPack<>;
template struct IntPack<1, 2, 3>;
template <class T, class U = int> struct Pair { T t; U u; T first() const { return t; } };
template struct Pair<char>;
template struct Pair<Pair<char, char>, Box<int>>;
struct Lit { int v; constexpr Lit(int x) : v(x) {} };
template <Lit L> struct ClassNTTP { static int get() { return L.v; } };
template struct ClassNTTP<Lit(7)>;
template <double D> struct DoubleT { static double get() { return D; } };
template struct DoubleT<1.5>;

template <class T> T twice(T t) { return t + t; }
template int twice<int>(int);
template double twice<double>(double);
template <class T, class U> auto addt(T t, U u) -> decltype(t + u) { return t + u; }
template <class T> auto autoret(T t) { return t; }
template <class T> decltype(auto) dautoret(T &t) { return (t); }
template <class... Ts> int variadic_t(Ts...) { return sizeof...(Ts); }
template int variadic_t<>();
template int variadic_t<int, char>(int, char);
template <int N> int arr_t(int (&)[N]) { return N; }
template int arr_t<3>(int (&)[3]);

template <class T> T tvar = T(1);
template int tvar<int>;
template double tvar<double>;

// ------------------------------------------------------------ statics, guards, init
struct NonTrivial {
    int v;
    NonTrivial(int x) : v(x) {}
    ~NonTrivial() { v = 0; }
};
NonTrivial g_nontrivial(f_void());
static NonTrivial s_nontrivial(f_void() + 1);
int static_local(int x)
{
    static int counter = x;
    static NonTrivial local_obj(x);
    struct Local {
        int l;
        int get() { return l; }
    };
    Local l{x};
    return counter++ + local_obj.v + l.get();
}
int static_local2()
{
    {
        static int a = f_void();
        {
            static int b = f_void() + a;
            return b;
        }
    }
}
extern "C" {
unsigned int _tls_index; /* normally from the CRT */
#if defined(_M_IX86)
unsigned int _tls_array;
#endif
}
__declspec(thread) int g_tls;
__declspec(thread) int g_tls2 = 1;
const char *g_str = "hello, names";
const wchar_t *g_wstr = L"wide names";
const char16_t *g_ustr = u"utf16";
const char32_t *g_Ustr = U"utf32";
const char8_t *g_u8str = u8"utf8";

// ------------------------------------------------------------ lambdas
int use_lambdas(int k)
{
    auto l1 = [](int a) { return a + 1; };
    auto l2 = [k](int a) { return a + k; };
    auto l3 = [&k]() mutable { return ++k; };
    auto gen = [](auto a) { return a; };
    int (*fp)(int) = l1;
    return l1(1) + l2(2) + l3() + gen(3) + (int)gen(4.0) + fp(5);
}

// ------------------------------------------------------------ misc
struct WithConst {
    static const int c = 5;
    static constexpr double d = 2.5;
    static inline int inl = 3;
};
const int WithConst::c;
const int *use_withconst() { return &WithConst::c + (int)WithConst::d + WithConst::inl; }

struct Conv {
    template <class T> operator T() const { return T(); }
};
int use_conv() { Conv c; int i = c; double d = c; return i + (int)d; }

struct Friendly {
    friend int friend_func(Friendly f) { return f.x; }
    int x;
};
int use_friend() { return friend_func(Friendly{1}); }

extern "C" int start()
{
    int r = f_void() + f_variadic(1, 2) + call_anon() + use_ops() + static_local(1) + static_local2();
    f_args(0, 0, 0, 0, 0, 0, 0, 0, 0);
    f_args2(0, 0, 0, 0, 0, 0, 0);
    f_args3(u8'a', u'b', U'c', nullptr);
    int i = 0;
    f_ptrs(&i, &i, &i, &i, &i, i, i, static_cast<int &&>(i));
    int a3[3], a23[2][3];
    f_arrays(a3, &a3, a3, a23);
    f_funcs(nullptr, nullptr, vfun, nullptr);
    f_backrefs(0, 0, 0, 0, 0, 0);
    r += *f_retcptr() + f_retcref() + f_retrref() + f_retv() + (f_retfp() != nullptr) + (f_retparr() != nullptr);
    f_noexcept();
    f_restrict(0, 0);
    f_unaligned(0);
#if defined(_M_IX86)
    r += f_stdcall(1, 2) + f_fastcall(1, 2) + c_stdcall(1, 2) + c_fastcall(1, 2);
#endif
    r += f_vectorcall(1, 2.0) + f_cdecl(1) + c_function(1) + c_data + c_vectorcall(1, 2);
    f_udts(Red, Small::A, Big::B, Neg::C, S{}, U{}, K{}, nullptr, U{}, K{});
    r += outer::inner::deep(1) + outer::inner::deep_data + outer::inner::Deep::Nested().get() + outer::versioned();
    Access acc;
    r += acc.pub(0) + acc.vpub() + acc.cpub() + acc.vlpub() + acc.cvpub() + acc.lref() + Access().rref() +
         acc.clref() + Access::sfunc(0) + acc.all() + Access::spub;
    Concrete conc;
    Abstract *ab = &conc;
    r += ab->pure();
    VC vc;
    VB *vbp = &vc;
    r += vbp->fb() + vbp->fboth() + vc.fa();
    VDiamond dia;
    r += dia.fv() + dia.d1 + dia.d2;
    VT vt;
    r += vt.fv();
    MP mp{};
    r += f_memptrs(&MP::x, &MP::f, &MP::cf, &MPVirt::n) + (mp.*g_mp_func)(1) + (mp.*g_mp_vfunc)(2) + mp.*g_mp_data;
    r += IntT<0>::get() + BoolT<true>::get() + Pack<int>::count() + IntPack<1, 2, 3>::count();
    r += twice(1) + (int)addt(1, 2.0) + autoret(3) + dautoret(i) + variadic_t(1, 'c') + arr_t(a3);
    r += tvar<int> + (int)tvar<double>;
    r += use_lambdas(4) + (int)(long long)use_withconst() + use_conv() + use_friend();
    r += g_nontrivial.v + s_nontrivial.v + g_tls + g_tls2 + (g_str != nullptr) + (g_wstr != nullptr);
    r += (g_ustr != nullptr) + (g_Ustr != nullptr) + (g_u8str != nullptr);
    return r;
}
