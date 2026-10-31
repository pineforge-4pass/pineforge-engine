// The including translation unit supplies the unchanged fixture entry point.
int main() {
    for (pineforge::WindowCacheCapacity capacity :
         {pineforge::WindowCacheCapacity{}, pineforge::WindowCacheCapacity{1},
          pineforge::WindowCacheCapacity{0}}) {
        pineforge::WindowCacheScope scope(capacity);
        const int result = pineforge_ta_fixture_main();
        if (result != 0) return result;
    }
    return 0;
}
