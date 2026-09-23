#include "storage/Page.hpp"

#include <cassert>

using namespace zenthrildb;

void test_page_format() {
  Page page;
  page.header().pageId = 7;
  page.header().usedBytes = 12;
  const auto bytes = page.serialize();
  Page restored;
  restored.deserialize(bytes);
  assert(restored.header().pageId == 7);
  assert(restored.header().usedBytes == 12);
}
