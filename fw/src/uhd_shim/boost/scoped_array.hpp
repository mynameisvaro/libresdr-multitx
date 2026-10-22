#pragma once
#include <cstddef>

namespace boost {
template <typename T> class scoped_array
{
public:
    explicit scoped_array(T* p = nullptr) : _p(p) {}
    ~scoped_array() { delete[] _p; }
    scoped_array(const scoped_array&) = delete;
    scoped_array& operator=(const scoped_array&) = delete;
    T& operator[](std::ptrdiff_t i) const { return _p[i]; }
    T* get() const { return _p; }

private:
    T* _p;
};
} // namespace boost
