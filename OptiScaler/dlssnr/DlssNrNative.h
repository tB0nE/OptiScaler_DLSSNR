#pragma once
#include <string>
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
std::string Status();
}
