vtk_add_test_cxx(${vtk-module}CxxTests tests
  #TestRenderWidget.cxx # Very experimental, fails, does nothing useful yet.
  TestPointGaussianMapper.cxx
  TestPointGaussianMapperOpacity.cxx
  TestVBOPLYMapper.cxx
  TestVBOPointsLines.cxx
  TestGaussianBlurPass.cxx
  TestBlurAndSobelPasses.cxx
  TestShadowMapBakerPass.cxx
  TestShadowMapPass.cxx
  TestSobelGradientMagnitudePass.cxx
  )

vtk_test_cxx_executable(${vtk-module}CxxTests tests RENDERING_FACTORY)
