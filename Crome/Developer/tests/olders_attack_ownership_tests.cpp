#include "../native_runtime/actions/OldersAttackOwnership.hpp"
#include <iostream>
#include <stdexcept>
struct Creature {std::uintptr_t vtable=1;volatile LONG references=1;};
struct Argument {Creature* object;std::uintptr_t extra;};
static_assert(offsetof(Creature,references)==crome::olders_attack::counterOffset);
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
// Model the observed native contract: by-value argument consumed on early and
// normal returns, including the Game target's separately retained ownership.
__attribute__((noinline)) static void nativeAttack(Creature*& gameTarget,Argument* argument,bool early) {
    Creature* object=argument->object;
    if(!early&&gameTarget!=object){InterlockedIncrement(&object->references);if(gameTarget)InterlockedDecrement(&gameTarget->references);gameTarget=object;}
    check(InterlockedDecrement(&object->references)>0,"map-owned creature released prematurely");
}
int main(){try{
    Creature a,b;Creature* gameTarget=nullptr;
    for(unsigned i=0;i<100;++i){auto* target=i%2?&a:&b;
        check(crome::olders_attack::acquireConsumedReference(&target->references),"live argument not acquired");
        Argument argument{target,reinterpret_cast<std::uintptr_t>(&target)};
        nativeAttack(gameTarget,&argument,i%3==0);
        check(a.references==(gameTarget==&a?2:1)&&b.references==(gameTarget==&b?2:1),"ownership leaked or decremented twice");
    }
    if(gameTarget)InterlockedDecrement(&gameTarget->references);
    check(a.references==1&&b.references==1,"map references unbalanced");
    volatile LONG invalid=0;check(!crome::olders_attack::acquireConsumedReference(&invalid)&&invalid==0,"removed creature resurrected");
    invalid=-1;check(!crome::olders_attack::acquireConsumedReference(&invalid),"negative refcount accepted");
    invalid=std::numeric_limits<LONG>::max();check(!crome::olders_attack::acquireConsumedReference(&invalid),"counter overflow accepted");
    std::cout<<"PASS: consumed attack argument, early return, target switching, repeated calls and invalid refcounts\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
