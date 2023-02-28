#pragma once

#include <Althea/Image.h>
#include <Althea/ImageView.h>
#include <Althea/Sampler.h>

namespace AltheaDemo {
struct ImageResource {
  AltheaEngine::Image image{};
  AltheaEngine::ImageView view{};
  AltheaEngine::Sampler sampler{};
};
} // namespace AltheaDemo