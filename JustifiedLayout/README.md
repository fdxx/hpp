C++ version of https://github.com/flickr/justified-layout

```c++
namespace jl = JustifiedLayout;

jl::LayoutConfig LayoutCfg;
LayoutCfg.containerWidth            = 1000;
LayoutCfg.targetRowHeight           = 500;
LayoutCfg.targetRowHeightTolerance  = 0.25;
LayoutCfg.containerPadding          = {0, 0, 0, 0};
LayoutCfg.boxSpacing                = {6, 6};
LayoutCfg.showWidows                = true;
LayoutCfg.widowLayoutStyle          = jl::WidowLayoutStyle::Left;

std::vector<jl::InputItem> items;
//items.emplace_back(ratio);
auto layout = jl::compute(items, LayoutCfg);

for (const auto& box : layout.boxes)
{
    std::println("{} {} {} {}", box.top, box.left, box.width, box.height);
}

```
