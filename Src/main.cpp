#include <Althea/Application.h>
#include "SponzaTest.h"
#include "GenIrradianceMap.h"

#include <iostream>

using namespace AltheaEngine;

int main() {
  Application app("../..", "../../Extern/Althea");
  app.createGame<GenIrradianceMap>();

  try {
    app.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}