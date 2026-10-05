// Must not compile. ctest compiles this file (CMakeLists.txt) and passes only on Publisher's
// static_assert: a BookDelta published without its snapshot would leave a consumer that recovers
// later trusting a snapshot that lacks the delta, so publish() and publish_with_latency_start()
// refuse one. -DMDBUS_WITH_LATENCY_START picks the second entry point.

#include "mdbus/publisher.hpp"

void publish_delta_without_snapshot(mdbus::Publisher<>& publisher, const mdbus::BookDelta& delta) {
#ifdef MDBUS_WITH_LATENCY_START
  publisher.publish_with_latency_start(0, delta, 1);
#else
  publisher.publish(0, delta);
#endif
}
