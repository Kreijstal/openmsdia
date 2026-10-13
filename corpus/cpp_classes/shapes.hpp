// OpenMSDIA test corpus: C++ classes. Freestanding, no CRT, no RTTI or EH.
#ifndef CORPUS_SHAPES_HPP
#define CORPUS_SHAPES_HPP

namespace geo {

enum class Kind : unsigned char { Circle = 1, Square = 2, Composite = 3 };

struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2 &o) const { return Vec2(x + o.x, y + o.y); }
    double dot(const Vec2 &o) const { return x * o.x + y * o.y; }
};

class Shape {
public:
    explicit Shape(Kind k) : kind_(k), id_(next_id++) {}
    virtual ~Shape();
    virtual double area() const = 0;
    virtual const char *name() const { return "shape"; }
    Kind kind() const { return kind_; }
    int id() const { return id_; }
    static int next_id;

protected:
    Kind kind_;

private:
    int id_;
};

class Circle : public Shape {
public:
    explicit Circle(double r) : Shape(Kind::Circle), radius_(r) {}
    double area() const override;
    const char *name() const override { return "circle"; }

private:
    double radius_;
};

class Square final : public Shape {
public:
    explicit Square(double s) : Shape(Kind::Square), side_(s) {}
    double area() const override { return side_ * side_; }

private:
    double side_;
};

struct Named {
    const char *label;
    virtual void rename(const char *l) { label = l; }
};

class LabelledCircle : public Circle, public Named {
public:
    LabelledCircle(double r, const char *l) : Circle(r) { label = l; }
    const char *name() const override { return label; }
};

struct VBase { int vb = 1; };
struct VLeft : virtual VBase { int l = 2; };
struct VRight : virtual VBase { int r = 3; };
struct Diamond : VLeft, VRight { int d = 4; };

template <typename T, int N>
struct FixedArray {
    T items[N];
    int size() const { return N; }
    T &operator[](int i) { return items[i]; }
    T sum() const {
        T s{};
        for (int i = 0; i < N; ++i)
            s = s + items[i];
        return s;
    }
};

template <typename T>
inline T max_of(T a, T b) { return a < b ? b : a; }

double total_area(Shape *const *shapes, int n);
int use_templates();

} // namespace geo

#endif
