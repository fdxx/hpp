C++ version of https://github.com/Roughsketch/imagesize

```c++
auto r = imgsize::from_file("photo.jpg");
if (r) std::println("{}x{}", r->width, r->height);
```
