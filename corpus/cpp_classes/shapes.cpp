#include "shapes.hpp"

namespace geo {

int Shape::next_id = 100;

Shape::~Shape() {}

double Circle::area() const
{
    const double pi = 3.14159;
    return pi * radius_ * radius_;
}

double total_area(Shape *const *shapes, int n)
{
    double total = 0;
    for (int i = 0; i < n; ++i) {
        Shape *s = shapes[i];
        total += s->area();
    }
    return total;
}

} // namespace geo
