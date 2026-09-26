#pragma once
#include "Types.h"
#include <wrl/client.h>
namespace sat {
using Microsoft::WRL::ComPtr;
class ComScope {
 HRESULT hr_;
public:
 ComScope():hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) { if(FAILED(hr_) && hr_!=RPC_E_CHANGED_MODE) CheckHR(hr_, "COM"); }
 ~ComScope(){ if(SUCCEEDED(hr_)) CoUninitialize(); }
};
}
