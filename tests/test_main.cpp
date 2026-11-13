#include <iostream>

void run_xspf_tests();
void run_soloist_protocol_tests();

int main() {
    run_xspf_tests();
    run_soloist_protocol_tests();
    std::cout << "All spotify_core unit tests passed.\n";
    return 0;
}
