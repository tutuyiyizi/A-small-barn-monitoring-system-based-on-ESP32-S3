#pragma once
#include "model.h"
bool networkBegin();
void networkSnapshot(const Snapshot &data);
bool networkCommand(Command &command);
void networkState(NetworkState &state);
