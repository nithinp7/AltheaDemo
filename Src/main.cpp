#include "DemoScene.h"
#include "RayTracingDemo.h"
#include "BindlessDemo.h"

#include <Althea/Application.h>

#include <iostream>

using namespace AltheaEngine;
using namespace AltheaDemo;

int main() {
  Application app("../..", "../../Extern/Althea");
  // app.createGame<DemoScene::DemoScene>();
  app.createGame<RayTracingDemo::RayTracingDemo>();
  // app.createGame<BindlessDemo::BindlessDemo>();

  try {
    app.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}