#include "common/Platform.h"
#include <iostream>
void RunCaptureTests();
void RunStoreTests();
void RunWorkerTests();
int main(){try{sat::ComScope com;RunCaptureTests();RunStoreTests();RunWorkerTests();std::cout<<"CoreTests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"CoreTests: FAIL: "<<e.what()<<"\n";return 1;}}
