#include "providers/Provider.h"
#include "settings/Store.h"
#include <iostream>
#include <algorithm>
// Manual diagnostic only: uses this application's saved credentials, never the desktop.
int main(int argc,char** argv) {
 if(argc!=2||std::string(argv[1])!="--saved-settings") {std::cerr<<"Use --saved-settings to test the configured API with a synthetic image.\n";return 2;}
 SetConsoleOutputCP(CP_UTF8);
 std::string key;
 auto clear=[&]{if(!key.empty())SecureZeroMemory(key.data(),key.size());};
 try {
  sat::Store store;auto options=store.LoadSettings();key=sat::UnprotectSecret(options.encryptedKey);
  if(key.empty()){std::cout<<"No saved API key; no request made.\n";return 2;}
  auto models=sat::FetchModels(options,key,{});
  std::cout<<"Models fetched: "<<models.size()<<"; saved model present: "<<(std::find(models.begin(),models.end(),options.model)!=models.end()?"yes":"no")<<std::endl;
  auto result=sat::TestVisionApi(options,key,{});
  std::cout<<"Vision API diagnostic PASS; recognized and translated synthetic image, segments: "<<result.segments.size()<<std::endl;
  clear();return 0;
 }catch(const sat::AppError& e){std::cerr<<"Diagnostic failed ["<<e.stage<<"]: "<<sat::Sanitize(e.what(),key)<<std::endl;clear();return 1;}
 catch(...){std::cerr<<"Diagnostic failed; no configuration was changed.\n";clear();return 1;}
}
