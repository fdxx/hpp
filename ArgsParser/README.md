ArgsParser

```c++
int main(int argc, char* argv[])
{
    ArgsParser args(argc, argv);
    auto v1 = args.Has("-k");
    auto v2 = args.Get<std::string>("--key");
    auto v3 = args.Get<int>("key");
}
```
