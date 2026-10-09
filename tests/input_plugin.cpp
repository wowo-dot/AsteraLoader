#include "al/al_plugin.h"
#include <windows.h>
#include <cstdint>

namespace {
const al_input_t* input{};
al_plugin_t* self{};
unsigned keys{},pads{},messages{};
bool valid=true;
void AL_CALL hotkey(al_plugin_t*,std::uint32_t,void*) {++keys;valid=valid && input->key_down(self,VK_F7) && input->key_down(self,VK_CONTROL);}
void AL_CALL pad_hotkey(al_plugin_t*,std::uint32_t,void*) {++pads;al_pad_state_t p{};p.size=sizeof(p);valid=valid && input->pad_state(self,0,&p)==AL_OK && p.buttons==AL_PAD_A;}
std::uint32_t AL_CALL message(al_plugin_t*,void* window,std::uint32_t id,std::uintptr_t,std::intptr_t,void*) {
    if(id==WM_CHAR) {++messages;valid=valid && window==input->window(self);return 1;}return 0;
}
void AL_CALL phase(al_plugin_t*,al_phase_t,const void*) {}
const al_plugin_info_t info{sizeof(info),AL_API_VERSION,"input_fixture","Input fixture","0.0.1",
    AL_PHASE_BIT(AL_PHASE_EXITING)|AL_PHASE_BIT(AL_PHASE_SHUTDOWN),0,nullptr,nullptr,nullptr,phase,nullptr};
}
extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info() {return &info;}
extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api,const al_init_t* init) {
    self=init->self;input=static_cast<const al_input_t*>(api->get_interface(self,AL_INPUT_INTERFACE,AL_INPUT_VERSION));
    if(!input || input->size!=sizeof(*input) || input->version!=AL_INPUT_VERSION ||
        api->get_interface(self,AL_INPUT_INTERFACE,AL_MAKE_VERSION(2,0)) ||
        !api->get_interface(self,AL_FRAME_INTERFACE,AL_FRAME_VERSION)) return AL_ERROR;
    const al_hotkey_t key{sizeof(key),VK_F7,AL_MOD_CTRL,0,"fixture"};
    const al_pad_hotkey_t pad{sizeof(pad),0,AL_PAD_A,0,"fixture pad"};
    if(!input->add_hotkey(self,&key,hotkey,nullptr) || !input->add_pad_hotkey(self,&pad,pad_hotkey,nullptr) ||
        input->subscribe_messages(self,0,message,nullptr)!=AL_OK || input->window(self)) return AL_ERROR;
    al_input_status_t status{};status.size=sizeof(status);input->status(self,&status);
    if(status.state!=AL_INPUT_WAITING || input->capture(self,AL_INPUT_ALL)!=AL_ERROR) return AL_ERROR;
    al_pad_state_t p{};p.size=sizeof(p);if(input->pad_state(self,4,&p)!=AL_ERROR) return AL_ERROR;
    return AL_OK;
}
extern "C" __declspec(dllexport) BOOL al_test_input_capture() {return input->capture(self,AL_INPUT_ALL)==AL_OK;}
extern "C" __declspec(dllexport) BOOL al_test_input_release() {return input->release(self)==AL_OK;}
extern "C" __declspec(dllexport) BOOL al_test_input_validate(unsigned k,unsigned p,unsigned m) {return valid && keys==k && pads==p && messages==m;}
