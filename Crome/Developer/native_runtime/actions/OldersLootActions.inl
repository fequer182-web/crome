// Included inside OldersInternalActions. Exact historical x64 baseline only.
// Container metadata offsets are confirmed by the matching ABIBuilder profile.
struct LootItemView {std::uintptr_t ptr{};unsigned id{},count{},index{};Position position{};};
struct LootContainerView {unsigned id{},capacity{};std::uintptr_t ptr{};LootItemView source;std::vector<LootItemView> items;};
OldersLootConfig lootConfig_{};OldersLootStep lootResult_{};
std::vector<std::pair<std::uintptr_t,std::uint64_t>> lootDone_;
std::uintptr_t lootOpeningGround_{};unsigned lootOpeningId_{};
std::uint64_t lootOpeningUntil_{};
std::vector<std::uintptr_t> lootBeforeOpenSources_;LootRetryBudget lootMoveRetries_;
std::uintptr_t lootApproachPtr_{};Position lootApproachPosition_{};unsigned lootApproachAttempts_{};

bool lootItemView(std::uintptr_t ptr,LootItemView& out) const {
    if(!ptr||!isReadableRange(ptr,0x40)||readPtr(ptr)!=base_+0x11DA0D8)return false;
    out.ptr=ptr;out.id=readU16(ptr+profile_.container.itemId);out.count=readU16(ptr+profile_.container.itemSubtype);
    out.position={static_cast<int>(readU32(ptr+0x18)),static_cast<int>(readU32(ptr+0x1C)),static_cast<int>(readU8(ptr+0x20))};
    return out.id!=0;
}
std::vector<LootContainerView> lootContainers() const {
    std::vector<LootContainerView> result;const auto& c=profile_.container;
    auto game=gameObject();if(!game)return result;
    auto header=readPtr(game+c.mapFromGame+c.mapHeader);
    if(!header||!isReadableRange(header,0x30))return result;
    std::vector<std::uintptr_t> stack{readPtr(header+c.nodeParent)},seen;
    while(!stack.empty()&&seen.size()<256){
        auto node=stack.back();stack.pop_back();
        if(!node||node==header||!isReadableRange(node,0x30)||std::find(seen.begin(),seen.end(),node)!=seen.end())continue;
        seen.push_back(node);if(readU8(node+c.nodeIsNil))continue;
        stack.push_back(readPtr(node+c.nodeLeft));stack.push_back(readPtr(node+c.nodeRight));
        auto ptr=readPtr(node+c.nodeContainer);if(!ptr||!isReadableRange(ptr,0x80))continue;
        LootContainerView view;view.ptr=ptr;view.id=readU32(ptr+c.containerId);view.capacity=readU32(ptr+0x1C);
        auto count=readU32(ptr+c.itemCount);
        if(view.id>63||!view.capacity||view.capacity>255||count>view.capacity||readU8(ptr+0x49)||readU8(ptr+0x4B)||readU32(ptr+0x50))continue;
        if(!lootItemView(readPtr(ptr+0x20),view.source))continue;
        if(!count){result.push_back(std::move(view));continue;}
        auto map=readPtr(ptr+c.dequeMap);std::size_t mapSize{},offset{};
        if(!safeRead(ptr+c.dequeMapSize,mapSize)||!safeRead(ptr+c.dequeOffset,offset)||!map||!mapSize||mapSize>65536||(mapSize&(mapSize-1)))continue;
        bool complete=true;
        for(unsigned i=0;i<count;++i){
            const auto logical=offset+i;const auto block=(logical>>1)&(mapSize-1);
            auto blockPtr=readPtr(map+block*sizeof(std::uintptr_t));LootItemView item;
            if(!lootItemView(readPtr(blockPtr+(logical&1)*sizeof(std::uintptr_t)),item)){complete=false;break;}
            item.index=i;view.items.push_back(item);
        }
        if(complete)result.push_back(std::move(view));
    }
    return result;
}
bool lootSource(const LootContainerView& view,const std::vector<LootContainerView>& all,int depth=0) const {
    if(view.id==lootConfig_.destination)return false;
    if(lootHas(lootConfig_.corpses,view.source.id))return true;
    if(depth>=3||!lootHas(lootConfig_.nested,view.source.id)||view.source.position.x!=65535||!(view.source.position.y&0x40))return false;
    auto parent=static_cast<unsigned>(view.source.position.y&0x3F);
    for(const auto& candidate:all)if(candidate.id==parent&&candidate.id!=view.id){
        for(const auto& item:candidate.items)if(item.id==view.source.id&&item.index==static_cast<unsigned>(view.source.position.z))
            return lootSource(candidate,all,depth+1);
    }
    return false;
}
bool lootWasDone(std::uintptr_t key,std::uint64_t now) {
    std::erase_if(lootDone_,[&](const auto& entry){return entry.second<=now;});
    return std::any_of(lootDone_.begin(),lootDone_.end(),[&](const auto& entry){return entry.first==key;});
}
void lootMarkDone(std::uintptr_t key,std::uint64_t now) {
    if(key&&!lootWasDone(key,now))lootDone_.push_back({key,now+30000});
    if(lootDone_.size()>256)lootDone_.erase(lootDone_.begin());
}
OldersLootStep executeLootStep() {
    if(!lootConfig_.enabled){lootDone_.clear();lootOpeningGround_=0;lootOpeningId_=0;lootBeforeOpenSources_.clear();lootMoveRetries_={};return {false,false,{},"disabled"};}
    if(!validateRuntime(false)||!profile_.actions.move||!profile_.actions.use||!mapRuntimeReady())return {false,false,{},"runtime_unavailable"};
    if(itemActionPending()||manualInteractionActive()||walkPending())return {false,false,{},"other_action_pending"};
    const auto now=GetTickCount64();const auto self=currentPosition();
    const auto all=lootContainers();const LootContainerView* destination=nullptr;
    for(const auto& container:all)if(container.id==lootConfig_.destination&&!lootHas(lootConfig_.corpses,container.source.id)){destination=&container;break;}
    if(!destination){
        std::string status="destination_bag_not_open id="+std::to_string(lootConfig_.destination)+" open=";
        for(const auto& container:all)status+=std::to_string(container.id)+"(item="+std::to_string(container.source.id)+") ";
        return {false,false,{},status};
    }
    if(destination->items.size()>=destination->capacity)return {false,false,{},"destination_bag_full"};
    const auto game=gameObject();
    for(const auto& corpse:all){
        if(!lootSource(corpse,all)||lootWasDone(corpse.source.ptr,now))continue;
        for(const auto& item:corpse.items){
            // Revalidate the container encoding used by native Game::move.
            if(item.position.x!=65535||item.position.y!=static_cast<int>(0x40|corpse.id)||item.position.z!=static_cast<int>(item.index)){
                lootMarkDone(corpse.source.ptr,now);return {false,false,{},"source_position_invalid"};
            }
            if(lootHas(lootConfig_.nested,item.id)){
                bool alreadyOpen=false;for(const auto& child:all)if(child.source.ptr==item.ptr||
                    (child.source.id==item.id&&child.source.position.x==65535&&child.source.position.y==item.position.y&&child.source.position.z==item.position.z))alreadyOpen=true;
                if(!alreadyOpen&&!lootWasDone(item.ptr,now)){
                    if(all.size()>=16)return {false,false,{},"too_many_open_containers"};
                    ThingHandle handle{item.ptr,0};handle.representation=reinterpret_cast<std::uintptr_t>(&handle.thing);
                    const auto fn=reinterpret_cast<GameUseFn>(base_+*profile_.actions.use);
                    fn(reinterpret_cast<void*>(game),&handle);lootMarkDone(item.ptr,now);
                    return {true,false,{},"opening_nested_bag"};
                }
                continue;
            }
            if(lootConfig_.onlyList&&!lootHas(lootConfig_.items,item.id))continue;
            if(!lootMoveRetries_.allow(item.ptr,item.count,corpse.id,item.index,destination->id)){
                lootMarkDone(corpse.source.ptr,now);lootMarkDone(lootOpeningGround_,now);
                lootOpeningGround_=0;lootOpeningId_=0;
                lootMoveRetries_={};return {false,false,{},"move_not_confirmed"};
            }
            auto pos=lootContainerDestination(destination->id,static_cast<unsigned>(destination->items.size()));
            NativePosition native{};native.x=pos.x;native.y=pos.y;native.z=static_cast<std::uint8_t>(pos.z);
            ThingHandle handle{item.ptr,0};handle.representation=reinterpret_cast<std::uintptr_t>(&handle.thing);
            using MoveFn=void(__fastcall*)(void*,ThingHandle*,NativePosition*,int);
            const auto fn=reinterpret_cast<MoveFn>(base_+*profile_.actions.move);
            std::cout<<"[LOOT] MOVE id="<<item.id<<" source="<<corpse.id<<":"<<item.index<<" destination="<<destination->id<<":"<<pos.z<<"\n"<<std::flush;
            fn(reinterpret_cast<void*>(game),&handle,&native,std::clamp(static_cast<int>(item.count),1,100));
            return {true,false,{},"move_requested"};
        }
        lootMarkDone(corpse.source.ptr,now);
        if(lootOpeningId_==corpse.source.id&&std::find(lootBeforeOpenSources_.begin(),lootBeforeOpenSources_.end(),corpse.source.ptr)==lootBeforeOpenSources_.end()){lootMarkDone(lootOpeningGround_,now);lootOpeningGround_=0;lootOpeningId_=0;}
    }
    if(lootOpeningGround_&&now<lootOpeningUntil_)return {true,false,{},"waiting_for_corpse_container"};
    if(lootOpeningGround_)lootMarkDone(lootOpeningGround_,now);
    lootOpeningGround_=0;lootOpeningId_=0;
    LootItemView best;Position bestPos{};int bestDistance=9999;
    for(int dy=-lootConfig_.radius;dy<=lootConfig_.radius;++dy)for(int dx=-lootConfig_.radius;dx<=lootConfig_.radius;++dx){
        Position pos{self.x+dx,self.y+dy,self.z};if(pos.x<=0||pos.y<=0||pos.x>=65535||pos.y>=65535)continue;
        NativePosition native{};native.x=pos.x;native.y=pos.y;native.z=static_cast<std::uint8_t>(pos.z);
        auto slot=getTileSlotGameThread(native);if(!slot||slot==base_+*profile_.runtime.mapEmptySlot)continue;
        auto tile=readPtr(slot);if(!tile||!isReadableRange(tile,0x60))continue;
        auto begin=readPtr(tile+profile_.map.tileThingsBegin),end=readPtr(tile+profile_.map.tileThingsEnd);
        if(!begin||end<begin||(end-begin)%8||(end-begin)>256*8||!isReadableRange(begin,end-begin))continue;
        for(auto cell=begin;cell<end;cell+=8){LootItemView item;if(!lootItemView(readPtr(cell),item)||!lootHas(lootConfig_.corpses,item.id)||lootWasDone(item.ptr,now))continue;
            if(item.position.x!=pos.x||item.position.y!=pos.y||item.position.z!=pos.z)continue;
            auto distance=self.chebyshevDistance(pos);if(distance<bestDistance){best=item;bestPos=pos;bestDistance=distance;}
        }
    }
    if(!best.ptr)return {false,false,{},"no_corpse"};
    if(bestDistance>1){
        if(lootApproachPtr_!=best.ptr||lootApproachPosition_.x!=self.x||lootApproachPosition_.y!=self.y||lootApproachPosition_.z!=self.z){
            lootApproachPtr_=best.ptr;lootApproachPosition_=self;lootApproachAttempts_=0;
        }
        if(++lootApproachAttempts_>5){lootMarkDone(best.ptr,now);lootApproachAttempts_=0;return {false,false,{},"corpse_unreachable"};}
        return {true,true,bestPos,"approaching_corpse"};
    }
    if(all.size()>=16)return {false,false,{},"too_many_open_containers"};
    lootBeforeOpenSources_.clear();for(const auto& container:all)lootBeforeOpenSources_.push_back(container.source.ptr);
    ThingHandle handle{best.ptr,0};handle.representation=reinterpret_cast<std::uintptr_t>(&handle.thing);
    const auto fn=reinterpret_cast<GameUseFn>(base_+*profile_.actions.use);
    fn(reinterpret_cast<void*>(game),&handle);
    lootOpeningGround_=best.ptr;lootOpeningId_=best.id;lootOpeningUntil_=now+2500;
    return {true,false,{},"opening_corpse"};
}
