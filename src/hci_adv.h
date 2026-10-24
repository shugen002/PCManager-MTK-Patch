#pragma once
// Put one 31-byte advertisement on the air through the MT7922.
namespace adv {

// Start (or replace) the advertised payload.  ad/len is the raw AD payload that
// PC Manager built for RtkApplSendAdv (<= 31 bytes); the worker keeps the newest
// one armed on the controller.  Safe to call from any thread, any number of times.
void Publish(const unsigned char* ad, int len);

}  // namespace adv
