#include <stdlib.h>
#include "MedianFilter.h"

using namespace xgm;

static int INT32CMP(const int32_t *a, const int32_t *b) {
  if (*a < *b)
    return -1;
  else if (*a> *b)
    return 1;
  return 0;
}

MedianFilter::MedianFilter(int tapSize) {
  tapSize_ = tapSize;
  tap_ = new int32_t[tapSize_];
}

MedianFilter::~MedianFilter() {
  delete [] tap_;
}

void MedianFilter::Reset() {
  for(int i=0; i<tapSize_; i++)
    tap_[i] = 0;
  dirty_ = false;
  tapIndex_ = 0;
}

void MedianFilter::Put(int32_t data) {
  tap_[tapIndex_++%tapSize_] = data;
  dirty_ = true;
}

int32_t MedianFilter::Get() {
  if(dirty_) {
    qsort(tap_, tapSize_, sizeof(int32_t), (int(*)(const void*, const void*))INT32CMP);
    dirty_ = false;
  }
  return tap_[tapSize_>>1];
}

