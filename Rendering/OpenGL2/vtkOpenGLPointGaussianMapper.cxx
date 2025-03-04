/*=========================================================================

  Program:   Visualization Toolkit

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkOpenGLPointGaussianMapper.h"

#include "vtkOpenGLHelper.h"

#include "vtkCellArray.h"
#include "vtkHardwareSelector.h"
#include "vtkMath.h"
#include "vtkObjectFactory.h"
#include "vtkMatrix4x4.h"
#include "vtkOpenGLActor.h"
#include "vtkOpenGLCamera.h"
#include "vtkOpenGLIndexBufferObject.h"
#include "vtkOpenGLPolyDataMapper.h"
#include "vtkOpenGLVertexArrayObject.h"
#include "vtkOpenGLVertexBufferObject.h"
#include "vtkPiecewiseFunction.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkProperty.h"
#include "vtkRenderer.h"
#include "vtkShaderProgram.h"

#include "vtkPointGaussianVS.h"
#include "vtkPolyDataFS.h"

#include "vtk_glew.h"



class vtkOpenGLPointGaussianMapperHelper : public vtkOpenGLPolyDataMapper
{
public:
  static vtkOpenGLPointGaussianMapperHelper* New();
  vtkTypeMacro(vtkOpenGLPointGaussianMapperHelper, vtkOpenGLPolyDataMapper)

  vtkPointGaussianMapper *Owner;

  bool UsingPoints;
  float *OpacityTable; // the table
  float OpacityScale; // used for quick lookups
  float OpacityOffset; // used for quick lookups

  float *ScaleTable; // the table
  float ScaleScale; // used for quick lookups
  float ScaleOffset; // used for quick lookups

protected:
  vtkOpenGLPointGaussianMapperHelper();
  ~vtkOpenGLPointGaussianMapperHelper();

  // Description:
  // Create the basic shaders before replacement
  virtual void GetShaderTemplate(
    std::map<vtkShader::Type, vtkShader *> shaders,
    vtkRenderer *, vtkActor *);

  // Description:
  // Perform string replacments on the shader templates
  virtual void ReplaceShaderColor(
    std::map<vtkShader::Type, vtkShader *> shaders,
    vtkRenderer *, vtkActor *);
  virtual void ReplaceShaderPositionVC(
    std::map<vtkShader::Type, vtkShader *> shaders,
    vtkRenderer *, vtkActor *);

  // Description:
  // Set the shader parameters related to the Camera
  virtual void SetCameraShaderParameters(vtkOpenGLHelper &cellBO, vtkRenderer *ren, vtkActor *act);

  // Description:
  // Set the shader parameters related to the actor/mapper
  virtual void SetMapperShaderParameters(vtkOpenGLHelper &cellBO, vtkRenderer *ren, vtkActor *act);

  // Description:
  // Does the VBO/IBO need to be rebuilt
  virtual bool GetNeedToRebuildBufferObjects(vtkRenderer *ren, vtkActor *act);

  // Description:
  // Update the VBO to contain point based values
  virtual void BuildBufferObjects(vtkRenderer *ren, vtkActor *act);

  virtual void RenderPieceDraw(vtkRenderer *ren, vtkActor *act);

  // create the table for opacity values
  void BuildOpacityTable(vtkPolyData *);

  // create the table for scale values
  void BuildScaleTable(vtkPolyData *);

  // Description:
  // Does the shader source need to be recomputed
  virtual bool GetNeedToRebuildShaders(vtkOpenGLHelper &cellBO,
    vtkRenderer *ren, vtkActor *act);

private:
  vtkOpenGLPointGaussianMapperHelper(const vtkOpenGLPointGaussianMapperHelper&); // Not implemented.
  void operator=(const vtkOpenGLPointGaussianMapperHelper&); // Not implemented.
};

//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkOpenGLPointGaussianMapperHelper)

//-----------------------------------------------------------------------------
vtkOpenGLPointGaussianMapperHelper::vtkOpenGLPointGaussianMapperHelper()
{
  this->Owner = NULL;
  this->OpacityTable = 0;
  this->ScaleTable = 0;
}


//-----------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::GetShaderTemplate(
  std::map<vtkShader::Type, vtkShader *> shaders,
  vtkRenderer *ren, vtkActor *actor)
{
  this->Superclass::GetShaderTemplate(shaders,ren,actor);

  if (this->Owner->GetScaleFactor() == 0.0)
    {
    this->UsingPoints = true;
    }
  else
    {
    this->UsingPoints = false;
    // for splats use a special shader than handles the offsets
    shaders[vtkShader::Vertex]->SetSource(vtkPointGaussianVS);
    }

}

void vtkOpenGLPointGaussianMapperHelper::ReplaceShaderPositionVC(
  std::map<vtkShader::Type, vtkShader *> shaders,
  vtkRenderer *ren, vtkActor *actor)
{
  if (!this->UsingPoints)
    {
    std::string VSSource = shaders[vtkShader::Vertex]->GetSource();
    std::string FSSource = shaders[vtkShader::Fragment]->GetSource();

    vtkShaderProgram::Substitute(FSSource,
      "//VTK::PositionVC::Dec",
      "varying vec2 offsetVCVSOutput;");

    vtkShaderProgram::Substitute(VSSource,
      "//VTK::Camera::Dec",
      "uniform mat4 VCDCMatrix;\n"
      "uniform mat4 MCVCMatrix;");

    shaders[vtkShader::Vertex]->SetSource(VSSource);
    shaders[vtkShader::Fragment]->SetSource(FSSource);
    }

  this->Superclass::ReplaceShaderPositionVC(shaders,ren,actor);
}

void vtkOpenGLPointGaussianMapperHelper::ReplaceShaderColor(
  std::map<vtkShader::Type, vtkShader *> shaders,
  vtkRenderer *ren, vtkActor *actor)
{
  if (!this->UsingPoints)
    {
    std::string FSSource = shaders[vtkShader::Fragment]->GetSource();

    if (this->Owner->GetSplatShaderCode())
      {
      vtkShaderProgram::Substitute(FSSource,"//VTK::Color::Impl",
        this->Owner->GetSplatShaderCode(), false);
      }
    else
      {
      vtkShaderProgram::Substitute(FSSource,"//VTK::Color::Impl",
        // compute the eye position and unit direction
        "//VTK::Color::Impl\n"
        "  float dist2 = dot(offsetVCVSOutput.xy,offsetVCVSOutput.xy);\n"
        "  if (dist2 > 9.0) { discard; }\n"
        "  float gaussian = exp(-0.5*dist2);\n"
        "  opacity = opacity*gaussian;"
        //  "  opacity = opacity*0.5;"
        , false);
      }
    shaders[vtkShader::Fragment]->SetSource(FSSource);
    }

  this->Superclass::ReplaceShaderColor(shaders,ren,actor);
  //cerr << shaders[vtkShader::Fragment]->GetSource() << endl;
}

//-----------------------------------------------------------------------------
bool vtkOpenGLPointGaussianMapperHelper::GetNeedToRebuildShaders(
  vtkOpenGLHelper &cellBO, vtkRenderer* ren, vtkActor *actor)
{
  this->LastLightComplexity = 0;

  vtkHardwareSelector* selector = ren->GetSelector();
  int picking = selector ? selector->GetCurrentPass() : -1;
  if (this->LastSelectionState != picking)
    {
    this->SelectionStateChanged.Modified();
    this->LastSelectionState = picking;
    }

  // has something changed that would require us to recreate the shader?
  // candidates are
  // property modified (representation interpolation and lighting)
  // input modified
  // light complexity changed
  if (cellBO.Program == 0 ||
      cellBO.ShaderSourceTime < this->GetMTime() ||
      cellBO.ShaderSourceTime < actor->GetMTime() ||
      cellBO.ShaderSourceTime < this->CurrentInput->GetMTime() ||
      cellBO.ShaderSourceTime < this->SelectionStateChanged ||
      cellBO.ShaderSourceTime < this->DepthPeelingChanged ||
      cellBO.ShaderSourceTime < this->LightComplexityChanged)
    {
    return true;
    }

  return false;
}

//-----------------------------------------------------------------------------
vtkOpenGLPointGaussianMapperHelper::~vtkOpenGLPointGaussianMapperHelper()
{
  if (this->OpacityTable)
    {
    delete [] this->OpacityTable;
    this->OpacityTable = 0;
    }
  if (this->ScaleTable)
    {
    delete [] this->ScaleTable;
    this->ScaleTable = 0;
    }
}

//-----------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::SetCameraShaderParameters(vtkOpenGLHelper &cellBO,
                                                    vtkRenderer* ren, vtkActor *actor)
{
  if (this->UsingPoints)
    {
    this->Superclass::SetCameraShaderParameters(cellBO,ren,actor);
    }
  else
    {
    vtkShaderProgram *program = cellBO.Program;

    vtkOpenGLCamera *cam = (vtkOpenGLCamera *)(ren->GetActiveCamera());

    vtkMatrix4x4 *wcdc;
    vtkMatrix4x4 *wcvc;
    vtkMatrix3x3 *norms;
    vtkMatrix4x4 *vcdc;
    cam->GetKeyMatrices(ren,wcvc,norms,vcdc,wcdc);
    program->SetUniformMatrix("VCDCMatrix", vcdc);

    if (!actor->GetIsIdentity())
      {
      vtkMatrix4x4 *mcwc;
      vtkMatrix3x3 *anorms;
      ((vtkOpenGLActor *)actor)->GetKeyMatrices(mcwc,anorms);
      vtkMatrix4x4::Multiply4x4(mcwc, wcvc, this->TempMatrix4);
      program->SetUniformMatrix("MCVCMatrix", this->TempMatrix4);
      }
    else
      {
      program->SetUniformMatrix("MCVCMatrix", wcvc);
      }

    // add in uniforms for parallel and distance
    cellBO.Program->SetUniformi("cameraParallel", cam->GetParallelProjection());
    }
}

//-----------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::SetMapperShaderParameters(vtkOpenGLHelper &cellBO,
                                                         vtkRenderer *ren, vtkActor *actor)
{
  if (!this->UsingPoints)
    {
    if (cellBO.IBO->IndexCount && (this->VBOBuildTime > cellBO.AttributeUpdateTime ||
        cellBO.ShaderSourceTime > cellBO.AttributeUpdateTime))
      {
      cellBO.VAO->Bind();
      if (!cellBO.VAO->AddAttributeArray(cellBO.Program, this->VBO,
                                      "offsetMC", this->VBO->ColorOffset+sizeof(float),
                                      this->VBO->Stride, VTK_FLOAT, 2, false))
        {
        vtkErrorMacro(<< "Error setting 'offsetMC' in shader VAO.");
        }
      }
    }

  this->Superclass::SetMapperShaderParameters(cellBO,ren,actor);
}


namespace
{

// internal function called by CreateVBO
// if verts are provided then we only draw those points
// otherwise we draw all the points
template< typename PointDataType, typename SizeDataType >
void vtkOpenGLPointGaussianMapperHelperPackVBOTemplate3(
              std::vector< float >::iterator& it,
              PointDataType* points,
              SizeDataType* sizes,
              vtkIdType index,
              vtkOpenGLPointGaussianMapperHelper *self,
              unsigned char *colors, int colorComponents,
              vtkDataArray *opacities, float defaultScale)
{
  PointDataType *pointPtr;
  unsigned char *colorPtr;

  unsigned char white[4] = {255, 255, 255, 255};
  vtkucfloat rcolor;

  // if there are no per point sizes and the default size is zero
  // then just render points, saving memory and speed
  pointPtr = points + index*3;
  colorPtr = colors ? (colors + index*colorComponents) : white;
  rcolor.c[0] = *(colorPtr++);
  rcolor.c[1] = *(colorPtr++);
  rcolor.c[2] = *(colorPtr++);

  if (opacities)
    {
    float opacity = opacities->GetComponent(index,0);
    if (self->OpacityTable)
      {
      float tindex = (opacity - self->OpacityOffset)*self->OpacityScale;
      int itindex = static_cast<int>(tindex);
      opacity = (1.0 - tindex + itindex)*self->OpacityTable[itindex] +
        (tindex - itindex)*self->OpacityTable[itindex+1];
      }
    rcolor.c[3] = opacity*255.0;
    }
  else
    {
    rcolor.c[3] = (colorComponents == 4 ? *colorPtr : 255);
    }

  if (self->UsingPoints)
    {
    // Vertices
    *(it++) = pointPtr[0];
    *(it++) = pointPtr[1];
    *(it++) = pointPtr[2];
    *(it++) = rcolor.f;
    }
  else // otherwise splats
    {
    float cos30 = cos(vtkMath::RadiansFromDegrees(30.0));

    float radius = sizes ? sizes[index] : 1.0;
    radius *= defaultScale;
    if (self->ScaleTable)
      {
      float tindex = (radius - self->ScaleOffset)*self->ScaleScale;
      int itindex = static_cast<int>(tindex);
      radius = (1.0 - tindex + itindex)*self->ScaleTable[itindex] +
        (tindex - itindex)*self->ScaleTable[itindex+1];
      }

    radius *= 3.0;

    // Vertices
    *(it++) = pointPtr[0];
    *(it++) = pointPtr[1];
    *(it++) = pointPtr[2];
    *(it++) = rcolor.f;
    *(it++) = -2.0f*radius*cos30;
    *(it++) = -radius;

    *(it++) = pointPtr[0];
    *(it++) = pointPtr[1];
    *(it++) = pointPtr[2];
    *(it++) = rcolor.f;
    *(it++) = 2.0f*radius*cos30;
    *(it++) = -radius;

    *(it++) = pointPtr[0];
    *(it++) = pointPtr[1];
    *(it++) = pointPtr[2];
    *(it++) = rcolor.f;
    *(it++) = 0.0f;
    *(it++) = 2.0f*radius;
    }
}

// internal function called by CreateVBO
// if verts are provided then we only draw those points
// otherwise we draw all the points
template< typename PointDataType, typename SizeDataType >
void vtkOpenGLPointGaussianMapperHelperPackVBOTemplate2(
              std::vector< float >::iterator& it,
              PointDataType* points, vtkIdType numPts,
              vtkOpenGLPointGaussianMapperHelper *self,
              vtkCellArray *verts,
              unsigned char *colors, int colorComponents,
              SizeDataType* sizes, vtkDataArray *opacities)
{
  float defaultSize = self->Owner->GetScaleFactor();

  // iterate over cells or not
  if (verts->GetNumberOfCells())
    {
    vtkIdType* indices(NULL);
    vtkIdType npts(0);
    for (verts->InitTraversal(); verts->GetNextCell(npts, indices); )
      {
      for (int i = 0; i < npts; ++i)
        {
        vtkOpenGLPointGaussianMapperHelperPackVBOTemplate3(
          it, points, sizes, indices[i], self,
          colors, colorComponents, opacities, defaultSize);
        }
      }
    }
  else
    {
    for (vtkIdType i = 0; i < numPts; ++i)
      {
      vtkOpenGLPointGaussianMapperHelperPackVBOTemplate3(
        it, points, sizes, i, self,
        colors, colorComponents, opacities, defaultSize);
      }
    }
}

template< typename PointDataType >
void vtkOpenGLPointGaussianMapperHelperPackVBOTemplate(
    std::vector< float >::iterator& it,
    PointDataType* points, vtkIdType numPts,
    vtkOpenGLPointGaussianMapperHelper *self,
    vtkCellArray *verts,
    unsigned char *colors, int colorComponents,
    vtkDataArray* sizes, vtkDataArray *opacities)
{
  if (sizes)
    {
    switch (sizes->GetDataType())
      {
      vtkTemplateMacro(
          vtkOpenGLPointGaussianMapperHelperPackVBOTemplate2(
            it, points, numPts,
            self, verts,
            colors, colorComponents,
            static_cast<VTK_TT*>(sizes->GetVoidPointer(0)), opacities)
          );
      }
    }
  else
    {
    vtkOpenGLPointGaussianMapperHelperPackVBOTemplate2(
          it, points, numPts,
          self, verts,
          colors, colorComponents,
          static_cast<float*>(NULL), opacities);
    }
}

} // anonymous namespace

//-------------------------------------------------------------------------
bool vtkOpenGLPointGaussianMapperHelper::GetNeedToRebuildBufferObjects(
  vtkRenderer *vtkNotUsed(ren),
  vtkActor *act)
{
  // picking state does not require a rebuild, unlike our parent
  if (this->VBOBuildTime < this->GetMTime() ||
      this->VBOBuildTime < act->GetMTime() ||
      this->VBOBuildTime < this->CurrentInput->GetMTime() ||
      this->VBOBuildTime < this->Owner->GetMTime() ||
      (this->Owner->GetScalarOpacityFunction() &&
        this->VBOBuildTime < this->Owner->GetScalarOpacityFunction()->GetMTime()) ||
      (this->Owner->GetScaleFunction() &&
        this->VBOBuildTime < this->Owner->GetScaleFunction()->GetMTime())
      )
    {
    return true;
    }
  return false;
}

//-------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::BuildOpacityTable(vtkPolyData *poly)
{
  vtkDataArray *oda =
    poly->GetPointData()->GetArray(this->Owner->GetOpacityArray());
  double range[2];
  oda->GetRange(range,0);

  // if a piecewise function was provided, use it to map the opacities
  vtkPiecewiseFunction *pwf = this->Owner->GetScalarOpacityFunction();
  int tableSize = this->Owner->GetOpacityTableSize();

  if (this->OpacityTable)
    {
    delete [] this->OpacityTable;
    }
  this->OpacityTable = new float [tableSize+1];
  if (pwf)
    {
    // build the interpolation table
    pwf->GetTable(range[0],range[1],tableSize,this->OpacityTable);
    // duplicate the last value for bilinear interp edge case
    this->OpacityTable[tableSize] = this->OpacityTable[tableSize-1];
    this->OpacityScale = (tableSize - 1.0)/(range[1] - range[0]);
    this->OpacityOffset = range[0];
    }

}

//-------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::BuildScaleTable(vtkPolyData *poly)
{
  vtkDataArray *oda =
    poly->GetPointData()->GetArray(this->Owner->GetScaleArray());
  double range[2];
  oda->GetRange(range,0);

  // if a piecewise function was provided, use it to map the opacities
  vtkPiecewiseFunction *pwf = this->Owner->GetScaleFunction();
  int tableSize = this->Owner->GetScaleTableSize();

  if (this->ScaleTable)
    {
    delete [] this->ScaleTable;
    }
  this->ScaleTable = new float [tableSize+1];
  if (pwf)
    {
    // build the interpolation table
    pwf->GetTable(range[0],range[1],tableSize,this->ScaleTable);
    // duplicate the last value for bilinear interp edge case
    this->ScaleTable[tableSize] = this->ScaleTable[tableSize-1];
    this->ScaleScale = (tableSize - 1.0)/(range[1] - range[0]);
    this->ScaleOffset = range[0];
    }
}

//-------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::BuildBufferObjects(
  vtkRenderer *vtkNotUsed(ren), vtkActor *vtkNotUsed(act))
{
  vtkPolyData *poly = this->CurrentInput;

  if (poly == NULL)
    {
    return;
    }

  bool hasScaleArray = this->Owner->GetScaleArray() != NULL &&
    poly->GetPointData()->HasArray(this->Owner->GetScaleArray());
  if (hasScaleArray && this->Owner->GetScaleFunction())
    {
    this->BuildScaleTable(poly);
    }
  else
    {
    if (this->ScaleTable)
      {
      delete [] this->ScaleTable;
      this->ScaleTable = 0;
      }
    }

  if (this->Owner->GetScaleFactor() == 0.0)
    {
    this->UsingPoints = true;
    }
  else
    {
    this->UsingPoints = false;
    }

  // if we have an opacity array then get it and if we have
  // a ScalarOpacityFunction map the array through it
  bool hasOpacityArray = this->Owner->GetOpacityArray() != NULL &&
    poly->GetPointData()->HasArray(this->Owner->GetOpacityArray());
  if (hasOpacityArray && this->Owner->GetScalarOpacityFunction())
    {
    this->BuildOpacityTable(poly);
    }
  else
    {
    if (this->OpacityTable)
      {
      delete [] this->OpacityTable;
      this->OpacityTable = 0;
      }
    }

  // For vertex coloring, this sets this->Colors as side effect.
  // For texture map coloring, this sets ColorCoordinates
  // and ColorTextureMap as a side effect.
  // I moved this out of the conditional because it is fast.
  // Color arrays are cached. If nothing has changed,
  // then the scalars do not have to be regenerted.
  this->MapScalars(1.0);

  // Figure out how big each block will be, currently 6 floats.
  int blockSize = 3;  // x y z
  this->VBO->VertexOffset = 0;
  this->VBO->NormalOffset = 0;
  this->VBO->TCoordOffset = 0;
  this->VBO->TCoordComponents = 0;
  this->VBO->ColorComponents = 4;
  this->VBO->ColorOffset = sizeof(float) * blockSize;
  ++blockSize; // color

  int splatCount = poly->GetPoints()->GetNumberOfPoints();
  if (poly->GetVerts()->GetNumberOfCells())
    {
    splatCount = poly->GetVerts()->GetNumberOfConnectivityEntries() -
      poly->GetVerts()->GetNumberOfCells();
    }
  if (!this->UsingPoints)
    {
    // two more floats
    blockSize += 2;  // offset
    splatCount *= 3;
    }

  this->VBO->PackedVBO.resize(blockSize * splatCount);
  this->VBO->Stride = sizeof(float) * blockSize;

  // Create a buffer, and copy the data over.
  std::vector<float>::iterator it = this->VBO->PackedVBO.begin();

  switch(poly->GetPoints()->GetDataType())
    {
    vtkTemplateMacro(
      vtkOpenGLPointGaussianMapperHelperPackVBOTemplate(
        it, static_cast<VTK_TT*>(poly->GetPoints()->GetVoidPointer(0)),
        poly->GetPoints()->GetNumberOfPoints(),
        this,
        poly->GetVerts(),
        this->Colors ? (unsigned char *)this->Colors->GetVoidPointer(0) : (unsigned char*)NULL,
        this->Colors ? this->Colors->GetNumberOfComponents() : 0,
        hasScaleArray ? poly->GetPointData()->GetArray(
          this->Owner->GetScaleArray()) : (vtkDataArray*)NULL,
        hasOpacityArray ? poly->GetPointData()->GetArray(
          this->Owner->GetOpacityArray()) : (vtkDataArray*)NULL
        ));
    }
  this->VBO->Upload(this->VBO->PackedVBO, vtkOpenGLBufferObject::ArrayBuffer);

  this->VBO->VertexCount = splatCount;

  // we use no IBO
  this->Points.IBO->IndexCount = 0;
  this->Lines.IBO->IndexCount = 0;
  this->TriStrips.IBO->IndexCount = 0;
  this->Tris.IBO->IndexCount = this->VBO->VertexCount;
}

//-----------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapperHelper::RenderPieceDraw(vtkRenderer* ren, vtkActor *actor)
{
  // draw polygons
  if (this->Owner->GetEmissive() != 0)
    {
    glBlendFunc( GL_SRC_ALPHA, GL_ONE);  // additive for emissive sources
    }
  if (this->VBO->VertexCount)
    {
    // First we do the triangles or points, update the shader, set uniforms, etc.
    this->UpdateShaders(this->Tris, ren, actor);
    if (this->UsingPoints)
      {
      glDrawArrays(GL_POINTS, 0,
        static_cast<GLuint>(this->VBO->VertexCount));
      }
    else
      {
      glDrawArrays(GL_TRIANGLES, 0,
        static_cast<GLuint>(this->VBO->VertexCount));
      }
    }
}


//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkOpenGLPointGaussianMapper)

//-----------------------------------------------------------------------------
vtkOpenGLPointGaussianMapper::vtkOpenGLPointGaussianMapper()
{
  this->Helper = vtkOpenGLPointGaussianMapperHelper::New();
  this->Helper->Owner = this;
}

vtkOpenGLPointGaussianMapper::~vtkOpenGLPointGaussianMapper()
{
  this->Helper->Delete();
  this->Helper = 0;
}

void vtkOpenGLPointGaussianMapper::RenderPiece(vtkRenderer *ren, vtkActor *act)
{
  if (this->GetMTime() > this->HelperUpdateTime)
    {
    this->Helper->vtkPolyDataMapper::ShallowCopy(this);
    this->HelperUpdateTime.Modified();
    }
  this->Helper->RenderPiece(ren,act);
}

//-----------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapper::ReleaseGraphicsResources(vtkWindow* win)
{
  this->Helper->ReleaseGraphicsResources(win);
  this->Helper->SetInputData(0);
  this->Modified();
}

//-----------------------------------------------------------------------------
bool vtkOpenGLPointGaussianMapper::GetIsOpaque()
{
  if (this->Emissive)
    {
    return false;
    }
  return this->Superclass::GetIsOpaque();
}

//-----------------------------------------------------------------------------
void vtkOpenGLPointGaussianMapper::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
