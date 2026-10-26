#include <iostream>
#include <exception>
void RunStoreTests();
int main() {
    try {
        RunStoreTests();
        std::cout << "Settings persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
