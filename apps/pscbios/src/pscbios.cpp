//
// PscBios: the running tool's shared part.
//
#include "pscbios.h"

using namespace std;

PscBios *PscBios::instance = nullptr;

//*******************************
// PscBios::PscBios
//*******************************
PscBios::PscBios(unique_ptr<ConsoleBackend> console) : console_(std::move(console)) {
    instance = this;
}

PscBios::~PscBios() {
    instance = nullptr;
}

//*******************************
// PscBios::get
//*******************************
PscBios &PscBios::get() {
    return *instance;
}
