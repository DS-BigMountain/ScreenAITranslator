void ServiceTests(){
 std::vector<TextRegion> regions{{1,{10,20,200,40},"HELLO",{{10,20,200,40}},20,{},.99f},{2,{10,60,200,80},"WORLD",{{10,60,200,80}},20,{},.99f}};
 const Json complete={{"source_language","en"},{"segments",Json::array({{{"source_id",1},{"translated_text","你好"}},{{"source_id",2},{"translated_text","世界"}}})}};
 Settings s;s.provider="deepl";s.baseUrl=Preset(s.provider).url;s.model="translate";
 Require(SelectTranslationConnection(s,regions).provider=="deepl","reliable OCR uses selected translation service");
 Reject([&]{SelectTranslationConnection(s,{});},"Vision");
 s.fallbackEnabled=true;s.fallbackProvider="gemini";s.fallbackModel="gemini-test";s.fallbackBaseUrl=L"https://generativelanguage.googleapis.com/v1beta";s.encryptedFallbackKey="protected-fallback";
 auto fallback=SelectTranslationConnection(s,{});Require(fallback.provider=="gemini"&&fallback.model=="gemini-test"&&fallback.encryptedKey==s.encryptedFallbackKey&&!fallback.fallbackEnabled,"fallback selects independent endpoint and credentials");
 for(const auto& preset:ProviderPresets())Require(CreateProvider(preset.id)!=nullptr,"every catalog entry has an implementation");
 Reject([]{CreateProvider("unsupported");},"settings");
 for(auto protocol:{ApiProtocol::Gemini,ApiProtocol::Anthropic}){
  s.provider=protocol==ApiProtocol::Gemini?"gemini":"anthropic";s.model="native-test";s.thinkingHigh=false;int calls=0;
  NativeProvider provider(protocol,[&](const Settings&,const std::string&,const std::string& body,Clock::time_point,std::stop_token,const std::wstring& path,const std::wstring& headers){
   ++calls;auto request=Json::parse(body);
   Require(body.find("image/jpeg")==std::string::npos,"native OCR text mode excludes image");
   if(protocol==ApiProtocol::Gemini){Require(path==L"/models/native-test:generateContent"&&headers==L"x-goog-api-key: test-key\r\n","Gemini route and authentication");Require(request.contains("system_instruction")&&request.contains("contents"),"Gemini protocol envelope");}
   else{Require(path==L"/messages"&&headers.find(L"anthropic-version: 2023-06-01")!=headers.npos&&headers.find(L"x-api-key: test-key")!=headers.npos,"Claude route and authentication");Require(request.contains("system")&&request.contains("max_tokens"),"Claude protocol envelope");}
   Json content=complete;if(calls==1)content["segments"].erase(1);
   Json envelope;
   if(protocol==ApiProtocol::Gemini){Json candidate;candidate["finishReason"]="STOP";candidate["content"]["parts"]=Json::array({{{"text",content.dump()}}});envelope["candidates"]=Json::array({candidate});}
   else{envelope["stop_reason"]="end_turn";envelope["content"]=Json::array({{{"type","text"},{"text",content.dump()}}});}
   return HttpResponse{200,envelope.dump()};
  });
  auto translated=provider.TranslateLocated({}, {},s,"test-key",{},regions);
  Require(calls==2&&translated.requestCount==2&&translated.segments[1].sourcePixels->top==60,"native missing-region retry retains geometry");
  auto vision=NativeLlmBody(protocol,"aW1n",{},s,{}).dump();Require(vision.find("image/jpeg")!=vision.npos&&vision.find("aW1n")!=vision.npos,"native vision payload encoding");
 }
 for(auto protocol:{ApiProtocol::DeepL,ApiProtocol::AzureTranslator}){
  s.provider=protocol==ApiProtocol::DeepL?"deepl":"azure-translator";s.serviceRegion="eastasia";int calls=0;
  NativeProvider provider(protocol,[&](const Settings&,const std::string&,const std::string& body,Clock::time_point,std::stop_token,const std::wstring& path,const std::wstring& headers){
   ++calls;auto request=Json::parse(body);
   if(protocol==ApiProtocol::DeepL){Require(request["text"].size()==2&&request["text"][0]=="HELLO"&&request["target_lang"]=="ZH-HANS","DeepL ordered batch");Require(headers==L"Authorization: DeepL-Auth-Key test-key\r\n"&&path==L"/translate","DeepL authentication");return HttpResponse{200,R"({"translations":[{"text":"你好"},{"text":"世界"}]})"};}
   Require(request.size()==2&&request[0]["Text"]=="HELLO"&&path==L"/translate?api-version=3.0&to=zh-Hans","Azure ordered batch and target language");Require(headers.find(L"Ocp-Apim-Subscription-Region: eastasia")!=headers.npos,"Azure resource region");return HttpResponse{200,R"([{"translations":[{"text":"你好"}]},{"translations":[{"text":"世界"}]}])"};
  });
  auto translated=provider.TranslateLocated({}, {},s,"test-key",{},regions);
  Require(calls==1&&translated.segments[1].sourceId==2&&translated.segments[1].translated=="世界","dedicated service preserves source mapping");
  Reject([&]{provider.Translate("aW1n",{},s,"test-key",{});},"Vision");
 }
 s.provider="deepl";
 NativeProvider incomplete(ApiProtocol::DeepL,[](auto&&...){return HttpResponse{200,R"({"translations":[{"text":"缺失"}]})"};});
 Reject([&]{incomplete.TranslateLocated({}, {},s,"test-key",{},regions);});
 std::vector<TextRegion> many;for(int i=0;i<123;++i){auto r=regions[0];r.id=i+1;many.push_back(r);}int batchCalls=0;
 NativeProvider batch(ApiProtocol::DeepL,[&](const Settings&,const std::string&,const std::string& body,auto,auto,auto,auto){++batchCalls;auto texts=Json::parse(body)["text"];Require(texts.size()<=50,"DeepL respects batch count");Json translations=Json::array();for(const auto& text:texts)translations.push_back({{"text","译文"}});return HttpResponse{200,Json{{"translations",translations}}.dump()};});
 Require(batch.TranslateLocated({}, {},s,"test-key",{},many).segments.size()==123&&batchCalls==3,"large screenshot split without loss");
 s.provider="qwen-mt";s.model="qwen-mt-turbo";std::atomic<int> active{},peak{};
 NativeProvider mt(ApiProtocol::QwenMt,[&](const Settings&,const std::string&,const std::string& body,auto,auto,auto,auto){
  auto count=++active;int old=peak.load();while(old<count&&!peak.compare_exchange_weak(old,count)){};
  auto request=Json::parse(body);if(request.contains("response_format")||request["messages"].size()!=1||request["translation_options"]["source_lang"]!="auto")throw std::runtime_error("Qwen MT protocol");
  auto text=request["messages"][0]["content"].get<std::string>();std::this_thread::sleep_for(std::chrono::milliseconds(5));--active;return HttpResponse{200,Envelope("译文"+text)};
 });
 auto mtResult=mt.TranslateLocated({}, {},s,"test-key",{},many);
 Require(mtResult.requestCount==123&&peak<=3&&mtResult.segments.back().sourceId==123,"Qwen MT bounded concurrency and ordering");
 s.provider="openai-compatible";int omittedCalls=0;
 CompatibleProvider omitted(false,[&](auto&&...){auto content=complete;if(++omittedCalls==1)content["segments"].erase(1);return HttpResponse{200,Envelope(content.dump())};});
 Require(omitted.TranslateLocated({}, {},s,"test-key",{},regions).segments.size()==2&&omittedCalls==2,"compatible missing-region retry");
 std::stop_source stop;stop.request_stop();bool cancelled=false;try{mt.TranslateLocated({}, {},s,"test-key",stop.get_token(),regions);}catch(const Cancelled&){cancelled=true;}Require(cancelled,"native cancellation before request");
 s.provider="gemini";s.model="native-test";int busyCalls=0;
 NativeProvider busy(ApiProtocol::Gemini,[&](auto&&...){++busyCalls;return HttpResponse{429,"{}",20};});
 Reject([&]{busy.TranslateLocated({}, {},s,"test-key",{},regions);},"Requesting");Require(busyCalls==1,"long Retry-After is not ignored");
 WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("WSAStartup");
 for(int mode:{1}){
  LocalServer server(0,200,false,R"({"translations":[{"text":"你好"},{"text":"世界"}]})");
  s.provider="deepl";s.proxyMode=mode;s.proxyAddress=L"127.0.0.1:"+std::to_wstring(server.port);
  s.baseUrl=mode==1?L"http://127.0.0.1:"+std::to_wstring(server.port)+L"/v2":L"http://127.0.0.1:1/v2";
  NativeProvider actual(ApiProtocol::DeepL);auto translated=actual.TranslateLocated({}, {},s,"native-secret",{},regions);
  Require(translated.segments.size()==2&&server.requestHeaders.find("Authorization: DeepL-Auth-Key native-secret")!=server.requestHeaders.npos,"native authentication reaches transport");
  Require(server.requestLine.find(mode==1?"POST /v2/translate ":"POST http://127.0.0.1:1/v2/translate ")==0,"direct and explicit proxy routing");
 }
 {LocalServer proxy(0,407,false,R"({"error":"proxy authentication required"})");s.proxyMode=2;s.proxyAddress=L"127.0.0.1:"+std::to_wstring(proxy.port);s.baseUrl=L"https://example.invalid/v2";
  NativeProvider actual(ApiProtocol::DeepL);Reject([&]{actual.TranslateLocated({}, {},s,"native-secret",{},regions);},"Requesting");Require(proxy.received&&proxy.requestLine.starts_with("CONNECT example.invalid:443 "),"explicit proxy receives HTTPS CONNECT");Require(proxy.requestHeaders.find("native-secret")==std::string::npos,"service credentials absent from CONNECT handshake");}
 s.proxyMode=1;s.provider="gemini";
 {LocalServer server(0,200,false,R"({"models":[{"name":"models/gemini-test","supportedGenerationMethods":["generateContent"]},{"name":"models/embedding-test","supportedGenerationMethods":["embedContent"]}]})");s.baseUrl=L"http://127.0.0.1:"+std::to_wstring(server.port)+L"/v1beta";auto models=FetchModels(s,"native-secret",{});Require(models.size()==1&&models[0]=="models/gemini-test"&&server.requestHeaders.find("x-goog-api-key: native-secret")!=server.requestHeaders.npos,"Gemini model discovery filters embedding models");}
 s.provider="deepl";
 {LocalServer server(0,200,false,R"({"translations":[{"text":"你好，世界 4826"}]})");s.baseUrl=L"http://127.0.0.1:"+std::to_wstring(server.port)+L"/v2";Require(TestTextApi(s,"native-secret",{}).segments.size()==1,"text-service connection diagnostic");}
 WSACleanup();
}
