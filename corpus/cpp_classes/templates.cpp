#include "shapes.hpp"

namespace geo {

namespace {
int hidden_counter;
int bump_hidden(int by) { return hidden_counter += by; }
}

FixedArray<int, 4> int_array = {{1, 2, 3, 4}};
FixedArray<Vec2, 2> vec_array;

int use_templates()
{
    vec_array[0] = Vec2(1, 2);
    vec_array[1] = Vec2(3, 4);
    Vec2 v = vec_array.sum();
    int m = max_of(int_array.sum(), int_array.size());
    double d = max_of(v.x, v.y);
    return bump_hidden(m + (int)d);
}

} // namespace geo
