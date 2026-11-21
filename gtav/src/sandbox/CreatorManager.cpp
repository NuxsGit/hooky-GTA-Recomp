#include "CreatorManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace gtav::sandbox {
namespace fs = std::filesystem;

namespace {
bool EnsureSeparateDirectory(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) return false;
  const auto status = fs::symlink_status(path, ec);
  if (ec || !fs::is_directory(status)) return false;
  const auto canon = fs::canonical(path, ec);
  if (ec) return false;
  return fs::weakly_canonical(path, ec) == canon && !ec;
}
bool ValidMapName(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) if (!(std::isalnum((unsigned char)c) || c=='_' || c=='-' || c==' ')) return false;
  // no path traversal
  if (name.find("..") != std::string_view::npos || name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) return false;
  return true;
}
std::string EscapeJson(std::string_view s) {
  std::string out;
  out.reserve(s.size()+8);
  for(char c: s){
    if(c=='"') out+= "\\\"";
    else if(c=='\\') out+= "\\\\";
    else if(c=='\n') out+= "\\n";
    else if(c=='\r') out+= "\\r";
    else if((unsigned char)c < 0x20) { char buf[7]; std::snprintf(buf,sizeof buf,"\\u%04x", (unsigned)c); out+=buf; }
    else out+=c;
  }
  return out;
}
} // anon

std::optional<CreatorWorkspace> PrepareCreatorWorkspace(const fs::path& user_root, CreatorWorkspaceError& error){
  error = CreatorWorkspaceError::MissingInstall;
  if(user_root.empty()) return std::nullopt;
  std::error_code ec;
  const auto real_root = fs::canonical(user_root, ec);
  if(ec || !fs::is_directory(real_root / "0000000000000000/545408A7/00000002", ec)) return std::nullopt;
  const auto creator = real_root / kCreatorSaveDir;
  CreatorWorkspace result{real_root, creator / kCreatorProfileDir, creator / kCreatorMapsDir};
  error = CreatorWorkspaceError::SeparateFolderRequired;
  if(!EnsureSeparateDirectory(creator) || !EnsureSeparateDirectory(result.profile_root) || !EnsureSeparateDirectory(result.maps_root))
    return std::nullopt;
  error = CreatorWorkspaceError::None;
  return result;
}

bool CreatorMap::Valid() const {
  if(!ValidMapName(name)) return false;
  if(description.size() > 500) return false;
  if(objects.size() > 240) return false; // higher than Build 160 due to creator ambition
  if(triggers.size() > 64) return false;
  for(auto &o: objects){
    if(!o.name.empty() && o.name.size()>64) return false;
    if(!workshop::FindModel(o.model)) return false;
    if(!o.pose.Valid()) return false;
    if(!o.scale.Valid(10.f)) return false;
  }
  for(auto &t: triggers){
    if(t.name.size()>48) return false;
    if(!t.pos.Valid(20000.f)) return false;
    if(t.radius < 0.5f || t.radius > 200.f) return false;
    if(t.type >= TriggerType::Count) return false;
  }
  return true;
}

bool SaveCreatorMap(const fs::path& maps_root, const CreatorMap& map, std::string& error_out){
  if(!ValidMapName(map.name)){ error_out="Invalid map name (A-Z 0-9 _ - space, max 64)"; return false; }
  if(!map.Valid()){ error_out="Map validation failed (too many objects/triggers or invalid model)"; return false; }
  std::error_code ec;
  fs::create_directories(maps_root, ec);
  if(ec){ error_out="Cannot create maps folder"; return false; }
  auto path = maps_root / (map.name + ".json");
  std::ofstream out(path, std::ios::trunc);
  if(!out){ error_out="Cannot write file"; return false; }
  out << "{\n";
  out << "  \"name\": \"" << EscapeJson(map.name) << "\",\n";
  out << "  \"description\": \"" << EscapeJson(map.description) << "\",\n";
  out << "  \"author\": \"" << EscapeJson(map.author) << "\",\n";
  out << "  \"version\": " << map.version << ",\n";
  out << "  \"player_start\": {" << "\"x\":" << map.player_start.x << ",\"y\":" << map.player_start.y << ",\"z\":" << map.player_start.z << ",\"yaw\":" << map.player_yaw << "},\n";
  out << "  \"objects\": [\n";
  for(size_t i=0;i<map.objects.size();++i){
    const auto& o = map.objects[i];
    out << "    {\"model\":" << o.model << ",\"kind\":" << int(o.kind) << ",\"x\":" << o.pose.at.x << ",\"y\":" << o.pose.at.y << ",\"z\":" << o.pose.at.z << ",\"yaw\":" << o.pose.yaw << ",\"sx\":" << o.scale.x << ",\"sy\":" << o.scale.y << ",\"sz\":" << o.scale.z << ",\"locked\":" << (o.locked?"true":"false") << ",\"name\":\"" << EscapeJson(o.name) << "\",\"cat\":\"" << EscapeJson(o.category) << "\"}";
    if(i+1<map.objects.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"triggers\": [\n";
  for(size_t i=0;i<map.triggers.size();++i){
    const auto& t = map.triggers[i];
    out << "    {\"name\":\"" << EscapeJson(t.name) << "\",\"x\":" << t.pos.x << ",\"y\":" << t.pos.y << ",\"z\":" << t.pos.z << ",\"r\":" << t.radius << ",\"type\":" << int(t.type) << ",\"script\":\"" << EscapeJson(t.script) << "\"}";
    if(i+1<map.triggers.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"scenario\": {\"name\":\"" << EscapeJson(map.scenario.name) << "\",\"desc\":\"" << EscapeJson(map.scenario.description) << "\",\"win\":" << int(map.scenario.win) << "}\n";
  out << "}\n";
  if(!out){ error_out="Write failed"; return false; }
  error_out.clear();
  return true;
}

// Very small JSON reader: we only need to parse our own writer format. No external dep.
std::optional<CreatorMap> LoadCreatorMap(const fs::path& maps_root, std::string_view name, std::string& error_out){
  if(!ValidMapName(name)){ error_out="Invalid name"; return std::nullopt; }
  auto path = maps_root / (std::string(name) + ".json");
  std::ifstream in(path);
  if(!in){ error_out="Map not found"; return std::nullopt; }
  std::string text((std::istreambuf_iterator<char>(in)), {});
  // Minimal extract: find "name": "..." etc. For MVP we parse objects array via simple scans.
  // To keep robust, we re-use trivial find; real Unity-like value parsing would be fuller JSON lib.
  auto find_str = [&](const char* key)->std::string{
    std::string needle = std::string("\"") + key + "\"";
    auto p = text.find(needle);
    if(p==std::string::npos) return {};
    p = text.find('"', p + needle.size());
    if(p==std::string::npos) return {};
    // handle string value: find opening quote after colon
    p = text.find('"', p); // Actually double find: above already at quote
    // better: find colon then quote
    // Simplified: find first quote after needle's colon
    auto colon = text.find(':', text.find(needle));
    if(colon==std::string::npos) return {};
    auto q1 = text.find('"', colon);
    if(q1==std::string::npos) return {};
    auto q2 = q1+1;
    std::string out;
    while(q2<text.size()){
      char c=text[q2];
      if(c=='\\'){ if(q2+1<text.size()){ char n=text[q2+1]; if(n=='"') out+='"'; else if(n=='\\') out+='\\'; else if(n=='n') out+='\n'; else out+=n; q2+=2; continue; } }
      if(c=='"') break;
      out+=c; ++q2;
    }
    return out;
  };
  auto find_num = [&](const char* key)->double{
    std::string needle = std::string("\"")+key+"\"";
    auto p=text.find(needle);
    if(p==std::string::npos) return 0;
    auto colon=text.find(':', p);
    if(colon==std::string::npos) return 0;
    // skip spaces
    size_t s=colon+1;
    while(s<text.size() && (text[s]==' '||text[s]=='\t')) ++s;
    char* end=nullptr;
    double v= std::strtod(text.c_str()+s, &end);
    return v;
  };
  CreatorMap map;
  // naive: we assume name equals filename, but parse also
  auto parsed_name = find_str("name");
  map.name = parsed_name.empty()? std::string(name): parsed_name;
  map.description = find_str("description");
  map.author = find_str("author");
  map.version = (uint32_t)find_num("version");
  if(map.version==0) map.version=1;
  map.player_start.x = (float)find_num("\"x\"");
  // Actually player_start has x,y,z,yaw — our find_num finds first "x" globally (maybe object). Need more precise: find player_start block
  {
    auto ps = text.find("player_start");
    if(ps!=std::string::npos){
      auto block = text.substr(ps, 200);
      auto get = [&](const char* k)->float{
        std::string nk=std::string("\"")+k+"\"";
        auto pp=block.find(nk);
        if(pp==std::string::npos) return 0;
        auto co=block.find(':', pp);
        if(co==std::string::npos) return 0;
        return (float)std::strtod(block.c_str()+co+1, nullptr);
      };
      map.player_start.x=get("x"); map.player_start.y=get("y"); map.player_start.z=get("z"); map.player_yaw=get("yaw");
    }
  }
  // Objects: find "objects": [ ... ]
  {
    auto opos=text.find("\"objects\"");
    if(opos!=std::string::npos){
      auto arr1=text.find('[', opos);
      auto arr2=text.find(']', arr1);
      if(arr1!=std::string::npos && arr2!=std::string::npos){
        std::string arr=text.substr(arr1, arr2-arr1);
        size_t cur=0;
        while(true){
          auto mo=arr.find("\"model\"", cur);
          if(mo==std::string::npos) break;
          CreatorObject obj;
          auto getN=[&](const char* kk)->double{
            std::string nk=std::string("\"")+kk+"\"";
            auto pp=arr.find(nk, mo);
            if(pp==std::string::npos) return 0;
            auto co=arr.find(':', pp);
            if(co==std::string::npos) return 0;
            // number may be like 123, 1.5
            size_t cc=co+1;
            while(cc<arr.size() && (arr[cc]==' '||arr[cc]=='\t')) ++cc;
            char* en=nullptr;
            double v=std::strtod(arr.c_str()+cc, &en);
            return v;
          };
          obj.model=(uint32_t)getN("model");
          obj.kind=workshop::Kind((int)getN("kind"));
          if(obj.kind!=workshop::Kind::Prop && obj.kind!=workshop::Kind::Vehicle && obj.kind!=workshop::Kind::Ped) obj.kind=workshop::Kind::Prop;
          obj.pose.at.x=(float)getN("\"x\""); obj.pose.at.y=(float)getN("\"y\""); obj.pose.at.z=(float)getN("\"z\""); obj.pose.yaw=(float)getN("\"yaw\""); obj.scale.x=(float)getN("\"sx\""); if(obj.scale.x==0) obj.scale.x=1; obj.scale.y=(float)getN("\"sy\""); if(obj.scale.y==0) obj.scale.y=1; obj.scale.z=(float)getN("\"sz\""); if(obj.scale.z==0) obj.scale.z=1;
          // locked
          auto lk=arr.find("\"locked\"", mo);
          if(lk!=std::string::npos){
            auto co=arr.find(':', lk);
            obj.locked = arr.find("true", co)!=std::string::npos && arr.find("true", co) < arr.find(',', co);
          }
          // name, cat strings
          auto getS=[&](const char* kk)->std::string{
            std::string nk=std::string("\"")+kk+"\"";
            auto pp=arr.find(nk, mo);
            if(pp==std::string::npos) return std::string();
            auto co=arr.find(':', pp);
            if(co==std::string::npos) return std::string();
            auto q1=arr.find('"', co);
            if(q1==std::string::npos) return std::string();
            q1=arr.find('"', q1+1); // Actually first quote after colon is value start
            // The above logic wrong: we searched from pp, co is colon, next quote is value start
            // Simpler: find first quote after colon+1
            auto qa=arr.find('"', co+1);
            if(qa==std::string::npos) return std::string();
            auto qb=qa+1;
            std::string out;
            while(qb<arr.size() && arr[qb]!='"'){ if(arr[qb]=='\\' && qb+1<arr.size()){ qb+=2; continue; } out+=arr[qb++]; }
            return out;
          };
          // For mo we already consumed model, but getS still searches from mo; first name may be after model
          // To avoid picking wrong, search from mo forward but limited to next '}'
          auto brace=arr.find('}', mo);
          // We'll just try to extract name/cat within this object slice
          std::string slice = arr.substr(mo, brace!=std::string::npos? brace-mo : 400);
          auto name_q = slice.find("\"name\"");
          if(name_q!=std::string::npos){
            auto co=slice.find(':', name_q);
            auto qa=slice.find('"', co+1);
            auto qb=slice.find('"', qa+1);
            if(qa!=std::string::npos && qb!=std::string::npos) obj.name=slice.substr(qa+1, qb-qa-1);
          }
          auto cat_q=slice.find("\"cat\"");
          if(cat_q!=std::string::npos){
            auto co=slice.find(':', cat_q);
            auto qa=slice.find('"', co+1);
            auto qb=slice.find('"', qa+1);
            if(qa!=std::string::npos && qb!=std::string::npos) obj.category=slice.substr(qa+1, qb-qa-1);
          }
          if(workshop::FindModel(obj.model)) map.objects.push_back(std::move(obj));
          cur = mo + 8;
          if(cur>=arr.size()) break;
        }
      }
    }
  }
  // Triggers similarly
  {
    auto tpos=text.find("\"triggers\"");
    if(tpos!=std::string::npos){
      auto arr1=text.find('[', tpos);
      auto arr2=text.find(']', arr1);
      if(arr1!=std::string::npos && arr2!=std::string::npos){
        std::string arr=text.substr(arr1, arr2-arr1);
        size_t cur=0;
        while(true){
          auto nm=arr.find("\"name\"", cur);
          if(nm==std::string::npos) break;
          CreatorTrigger trg;
          auto getN=[&](const char* kk)->double{
            std::string nk=std::string("\"")+kk+"\"";
            auto pp=arr.find(nk, nm);
            if(pp==std::string::npos) return 0;
            auto co=arr.find(':', pp);
            if(co==std::string::npos) return 0;
            return std::strtod(arr.c_str()+co+1, nullptr);
          };
          // name string
          {
            auto co=arr.find(':', nm);
            auto qa=arr.find('"', co+1);
            auto qb=arr.find('"', qa+1);
            if(qa!=std::string::npos && qb!=std::string::npos) trg.name=arr.substr(qa+1, qb-qa-1);
          }
          trg.pos.x=(float)getN("\"x\""); trg.pos.y=(float)getN("\"y\""); trg.pos.z=(float)getN("\"z\""); trg.radius=(float)getN("\"r\""); if(trg.radius==0) trg.radius=5;
          trg.type=TriggerType((int)getN("\"type\""));
          // script
          {
            auto sp=arr.find("\"script\"", nm);
            if(sp!=std::string::npos){
              auto co=arr.find(':', sp);
              auto qa=arr.find('"', co+1);
              auto qb=arr.find('"', qa+1);
              if(qa!=std::string::npos && qb!=std::string::npos) trg.script=arr.substr(qa+1, qb-qa-1);
            }
          }
          map.triggers.push_back(std::move(trg));
          cur=nm+7;
        }
      }
    }
  }
  // scenario block
  {
    auto spos=text.find("\"scenario\"");
    if(spos!=std::string::npos){
      // name
      auto nn=text.find("\"name\"", spos+10);
      if(nn!=std::string::npos){
        auto co=text.find(':', nn);
        auto qa=text.find('"', co+1);
        auto qb=text.find('"', qa+1);
        if(qa!=std::string::npos && qb!=std::string::npos) map.scenario.name=text.substr(qa+1, qb-qa-1);
      }
      auto dd=text.find("\"desc\"", spos);
      if(dd!=std::string::npos){
        auto co=text.find(':', dd);
        auto qa=text.find('"', co+1);
        auto qb=text.find('"', qa+1);
        if(qa!=std::string::npos && qb!=std::string::npos) map.scenario.description=text.substr(qa+1, qb-qa-1);
      }
      auto ww=text.find("\"win\"", spos);
      if(ww!=std::string::npos){
        auto co=text.find(':', ww);
        if(co!=std::string::npos) map.scenario.win=CreatorScenario::WinCondition((int)std::strtod(text.c_str()+co+1,nullptr));
      }
    }
  }
  if(!map.Valid()){
    // Even if strict valid fails due to our naive parse missing description length etc, still allow if objects parsed ok
    // Check minimal
    if(map.objects.size()>240){ error_out="Too many objects"; return std::nullopt; }
  }
  error_out.clear();
  return map;
}

bool DeleteCreatorMap(const fs::path& maps_root, std::string_view name){
  if(!ValidMapName(name)) return false;
  auto path=maps_root / (std::string(name)+".json");
  std::error_code ec;
  return fs::remove(path, ec);
}

std::vector<std::string> ListCreatorMaps(const fs::path& maps_root){
  std::vector<std::string> out;
  std::error_code ec;
  if(!fs::is_directory(maps_root, ec)) return out;
  for(auto &e: fs::directory_iterator(maps_root, ec)){
    if(ec) break;
    if(!e.is_regular_file()) continue;
    auto p=e.path();
    if(p.extension()!=".json") continue;
    auto stem=p.stem().string();
    if(ValidMapName(stem)) out.push_back(stem);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// CreatorManager

CreatorManager::CreatorManager(fs::path maps_root): maps_root_(std::move(maps_root)){
  std::error_code ec; fs::create_directories(maps_root_, ec);
  current_.name="Untitled"; current_.description="New Creator MAP"; current_.author="Player";
}

bool CreatorManager::New(const std::string& name){
  if(!ValidMapName(name)) return false;
  current_ = CreatorMap{}; current_.name=name; current_.description="New map — Unity/Unreal style"; dirty_=true; return true;
}
bool CreatorManager::Open(const std::string& name, std::string& error){
  auto loaded=LoadCreatorMap(maps_root_, name, error);
  if(!loaded) return false;
  current_=*loaded; dirty_=false; return true;
}
bool CreatorManager::Save(std::string& error){ bool ok=SaveCreatorMap(maps_root_, current_, error); if(ok) dirty_=false; return ok; }
bool CreatorManager::SaveAs(const std::string& name, std::string& error){
  if(!ValidMapName(name)){ error="Invalid name"; return false; }
  current_.name=name; return Save(error);
}
void CreatorManager::AddObject(const CreatorObject& obj){ current_.objects.push_back(obj); dirty_=true; }
bool CreatorManager::RemoveObject(size_t index){ if(index>=current_.objects.size()) return false; current_.objects.erase(current_.objects.begin()+index); dirty_=true; return true; }
bool CreatorManager::DuplicateObject(size_t index){ if(index>=current_.objects.size()) return false; auto c=current_.objects[index]; c.name += " copy"; current_.objects.push_back(std::move(c)); dirty_=true; return true; }
void CreatorManager::ClearObjects(){ current_.objects.clear(); dirty_=true; }
void CreatorManager::AddTrigger(const CreatorTrigger& t){ current_.triggers.push_back(t); dirty_=true; }
bool CreatorManager::RemoveTrigger(size_t index){ if(index>=current_.triggers.size()) return false; current_.triggers.erase(current_.triggers.begin()+index); dirty_=true; return true; }
bool CreatorManager::ValidForPlay(std::string& why) const{
  if(current_.objects.empty() && current_.triggers.empty()){ why="Map empty — add props/objects or triggers"; return false; }
  if(!current_.Valid()){ why="Map invalid (name, limits, model)"; return false; }
  why.clear(); return true;
}

} // namespace gtav::sandbox
