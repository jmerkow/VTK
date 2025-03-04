/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkShadowMapBakerPass.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkShadowMapBakerPass.h"
#include "vtkObjectFactory.h"
#include <cassert>

#include "vtkRenderState.h"
#include "vtkOpenGLRenderer.h"
#include "vtkgl.h"
#include "vtkFrameBufferObject.h"
#include "vtkTextureObject.h"
#include "vtkShaderProgram2.h"
#include "vtkOpenGLRenderWindow.h"
#include "vtkOpenGLError.h"
#include "vtkInformationIntegerKey.h"
#include "vtkMath.h"

// to be able to dump intermediate passes into png files for debugging.
// only for vtkShadowMapBakerPass developers.
//#define VTK_SHADOW_MAP_BAKER_PASS_DEBUG
//#define DONT_DUPLICATE_LIGHTS

#include "vtkImageImport.h"
#include "vtkPixelBufferObject.h"
#include "vtkImageExtractComponents.h"
#include "vtkLightCollection.h"
#include "vtkLight.h"
#include "vtkInformation.h"
#include "vtkCamera.h"
#include "vtkAbstractTransform.h" // for helper classes stack and concatenation
#include "vtkPerspectiveTransform.h"
#include "vtkTransform.h"

#include <vtksys/ios/sstream>
#include "vtkStdString.h"

// For vtkShadowMapBakerPassTextures, vtkShadowMapBakerPassLightCameras
#include "vtkShadowMapPassInternal.h"

// debugging
#include "vtkOpenGLState.h"
#include "vtkTimerLog.h"


vtkStandardNewMacro(vtkShadowMapBakerPass);
vtkCxxSetObjectMacro(vtkShadowMapBakerPass,OpaquePass,vtkRenderPass);
vtkCxxSetObjectMacro(vtkShadowMapBakerPass,CompositeZPass,vtkRenderPass);

vtkInformationKeyMacro(vtkShadowMapBakerPass,OCCLUDER,Integer);
vtkInformationKeyMacro(vtkShadowMapBakerPass,RECEIVER,Integer);

// ----------------------------------------------------------------------------
// helper function to compute the mNearest point in a given direction.
// To be called several times, with initialized = false the first time.
void vtkShadowMapBakerPass::PointNearFar(double *v,
                                         double *pt,
                                         double *dir,
                                         double &mNear,
                                         double &mFar,
                                         bool initialized)
{
  double diff[3];
  diff[0] =  v[0] - pt[0]; diff[1] =  v[1] - pt[1]; diff[2] =  v[2] - pt[2];
  double dot = vtkMath::Dot(diff, dir);
  if(initialized)
    {
    if(dot < mNear)
      {
      mNear = dot;
      }
    if(dot > mFar)
      {
      mFar = dot;
      }
    }
  else
    {
    mNear = dot;
    mFar = dot;
    }
}

// ----------------------------------------------------------------------------
// compute the min/max of the projection of a box in a given direction.
void vtkShadowMapBakerPass::BoxNearFar(double *bb,
                                       double *pt,
                                       double *dir,
                                       double &mNear,
                                       double &mFar)
{
  double v[3];
  v[0] = bb[0]; v[1] = bb[2]; v[2] = bb[4];
  PointNearFar(v, pt, dir, mNear, mFar, false);

  v[0] = bb[1]; v[1] = bb[2]; v[2] = bb[4];
  PointNearFar(v, pt, dir, mNear, mFar, true);

  v[0] = bb[0]; v[1] = bb[3]; v[2] = bb[4];
  PointNearFar(v, pt, dir, mNear, mFar, true);

  v[0] = bb[1]; v[1] = bb[3]; v[2] = bb[4];
  PointNearFar(v, pt, dir, mNear, mFar, true);

  v[0] = bb[0]; v[1] = bb[2]; v[2] = bb[5];
  PointNearFar(v, pt, dir, mNear, mFar, true);

  v[0] = bb[1]; v[1] = bb[2]; v[2] = bb[5];
  PointNearFar(v, pt, dir, mNear, mFar, true);

  v[0] = bb[0]; v[1] = bb[3]; v[2] = bb[5];
  PointNearFar(v, pt, dir, mNear, mFar, true);

  v[0] = bb[1]; v[1] = bb[3]; v[2] = bb[5];
  PointNearFar(v, pt, dir, mNear, mFar, true);
}

// ----------------------------------------------------------------------------
vtkShadowMapBakerPass::vtkShadowMapBakerPass()
{
  this->OpaquePass=0;
  this->CompositeZPass=0;

  this->Resolution=256;

  this->PolygonOffsetFactor=1.1f;
  this->PolygonOffsetUnits=4.0f;

  this->FrameBufferObject=0;
  this->ShadowMaps=0;
  this->LightCameras=0;

  this->HasShadows=false;
  this->NeedUpdate=true;
}

// ----------------------------------------------------------------------------
vtkShadowMapBakerPass::~vtkShadowMapBakerPass()
{
  if(this->OpaquePass!=0)
    {
    this->OpaquePass->Delete();
    }

  if(this->CompositeZPass!=0)
   {
   this->CompositeZPass->Delete();
   }

  if(this->FrameBufferObject!=0)
    {
    vtkErrorMacro(<<"FrameBufferObject should have been deleted in ReleaseGraphicsResources().");
    }

  if(this->ShadowMaps!=0)
    {
    vtkErrorMacro(<<"ShadowMaps should have been deleted in ReleaseGraphicsResources().");
    }
  if(this->LightCameras!=0)
    {
    vtkErrorMacro(<<"LightCameras should have been deleted in ReleaseGraphicsResources().");
    }
}

// ----------------------------------------------------------------------------
void vtkShadowMapBakerPass::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "OpaquePass: ";
  if(this->OpaquePass!=0)
    {
    this->OpaquePass->PrintSelf(os,indent);
    }
  else
    {
    os << "(none)" <<endl;
    }

  os << indent << "CompositeZPass: ";
  if(this->CompositeZPass!=0)
    {
    this->CompositeZPass->PrintSelf(os,indent);
    }
  else
    {
    os << "(none)" <<endl;
    }

  os << indent << "Resolution: " << this->Resolution << endl;

  os << indent << "PolygonOffsetFactor: " <<  this->PolygonOffsetFactor
     << endl;
  os << indent << "PolygonOffsetUnits: " << this->PolygonOffsetUnits << endl;
}

// ----------------------------------------------------------------------------
bool vtkShadowMapBakerPass::GetHasShadows()
{
  return this->HasShadows;
}

// ----------------------------------------------------------------------------
bool vtkShadowMapBakerPass::LightCreatesShadow(vtkLight *l)
{
  assert("pre: l_exists" && l!=0);

  return !l->LightTypeIsHeadlight() &&
    (!l->GetPositional() || l->GetConeAngle()<180.0);
}

// ----------------------------------------------------------------------------
vtkShadowMapBakerPassTextures *vtkShadowMapBakerPass::GetShadowMaps()
{
  return this->ShadowMaps;
}

// ----------------------------------------------------------------------------
vtkShadowMapBakerPassLightCameras *vtkShadowMapBakerPass::GetLightCameras()
{
  return this->LightCameras;
}

// ----------------------------------------------------------------------------
bool vtkShadowMapBakerPass::GetNeedUpdate()
{
  return this->NeedUpdate;
}

// ----------------------------------------------------------------------------
void vtkShadowMapBakerPass::SetUpToDate()
{
  this->NeedUpdate=false;
}

// ----------------------------------------------------------------------------
// Description:
// Perform rendering according to a render state \p s.
// \pre s_exists: s!=0
void vtkShadowMapBakerPass::Render(const vtkRenderState *s)
{
  assert("pre: s_exists" && s!=0);

  vtkOpenGLClearErrorMacro();

  this->NumberOfRenderedProps=0;
  this->HasShadows=false;

  vtkOpenGLRenderer *r=static_cast<vtkOpenGLRenderer *>(s->GetRenderer());
  vtkOpenGLRenderWindow *context=static_cast<vtkOpenGLRenderWindow *>(
    r->GetRenderWindow());
#ifdef VTK_SHADOW_MAP_BAKER_PASS_DEBUG
  vtkOpenGLState *state=new vtkOpenGLState(context);
#endif

  if(this->OpaquePass!=0)
    {
    // Disable the scissor test during the shadow map pass.
    GLboolean saved_scissor_test;
    glGetBooleanv(GL_SCISSOR_TEST, &saved_scissor_test);
    glDisable(GL_SCISSOR_TEST);

    // Test for Hardware support. If not supported, just render the delegate.
    bool supported=vtkFrameBufferObject::IsSupported(r->GetRenderWindow());

    if(!supported)
      {
      vtkErrorMacro("FBOs are not supported by the context. Cannot use shadow mapping.");
      }
    if(supported)
      {
      supported=vtkTextureObject::IsSupported(r->GetRenderWindow());
      if(!supported)
        {
        vtkErrorMacro("Texture Objects are not supported by the context. Cannot use shadow mapping.");
        }
      }

    if(supported)
      {
      supported=
        vtkShaderProgram2::IsSupported(static_cast<vtkOpenGLRenderWindow *>(
                                         r->GetRenderWindow()));
      if(!supported)
        {
        vtkErrorMacro("GLSL is not supported by the context. Cannot use shadow mapping.");
        }
      }

    if(!supported)
      {
      // Nothing to bake.
      return;
      }

    // Shadow mapping requires:
    // 1. at least one spotlight, not front light
    // 2. at least one receiver, in the list of visible props after culling
    // 3. at least one occluder, in the list of visible props before culling

    vtkLightCollection *lights=r->GetLights();
    lights->InitTraversal();
    vtkLight *l=lights->GetNextItem();
    bool hasLight=false;
    bool hasReceiver=false;
    bool hasOccluder=false;
    while(!hasLight && l!=0)
      {
      hasLight=l->GetSwitch() && this->LightCreatesShadow(l);
      l=lights->GetNextItem();
      }

    int propArrayCount=0;
    vtkProp **propArray=0;
    unsigned long latestPropTime=0;

    vtkInformation *requiredKeys=0;
    if(hasLight)
      {
      // at least one receiver?
      requiredKeys=vtkInformation::New();
      requiredKeys->Set(vtkShadowMapBakerPass::RECEIVER(),0); // dummy val.

      int i=0;
      int count=s->GetPropArrayCount();
      while(!hasReceiver && i<count)
        {
        hasReceiver=s->GetPropArray()[i]->HasKeys(requiredKeys);
        ++i;
        }
      if(hasReceiver)
        {
        requiredKeys->Remove(vtkShadowMapBakerPass::RECEIVER());
        requiredKeys->Set(vtkShadowMapBakerPass::OCCLUDER(),0); // dummy val.

        // at least one occluder?

        vtkCollectionSimpleIterator pit;
        vtkPropCollection *props=r->GetViewProps();
        props->InitTraversal(pit);
        vtkProp *p=props->GetNextProp(pit);
        propArray=new vtkProp*[props->GetNumberOfItems()];
        while(p!=0)
          {
          unsigned long mTime=p->GetMTime();
          if(latestPropTime<mTime)
            {
            latestPropTime=mTime;
            }
          if(p->GetVisibility())
            {
            propArray[propArrayCount]=p;
            ++propArrayCount;
            hasOccluder|=p->HasKeys(requiredKeys);
            }
          p=props->GetNextProp(pit);
          }
        }
      }
    this->HasShadows=hasOccluder;
    if(!hasOccluder)
      {
      // No shadows.
      if(requiredKeys!=0)
        {
        requiredKeys->Delete();
        }
#ifdef VTK_SHADOW_MAP_BAKER_PASS_DEBUG
      if(!hasLight)
        {
        cout << "no spotlight" << endl;
        }
      else
        {

        if(!hasReceiver)
          {
          cout << "no receiver" << endl;
          }
        else
          {
          cout << "no occluder" << endl;
          }
        }
#endif
      delete[] propArray;

      // Nothing to bake.
      return;
      }

    // Shadow mapping starts here.
    // 1. Create a shadow map for each spotlight.

    // Do we need to recreate shadow maps?
    this->NeedUpdate=this->LastRenderTime<lights->GetMTime();
    if(!this->NeedUpdate)
      {
      lights->InitTraversal();
      l=lights->GetNextItem();
      while(!this->NeedUpdate && l!=0)
        {
        // comparison should be this->LastRenderTime<l->GetMTime() but
        // we modify the lights during rendering (enable/disable state)
        // so cannot rely on this time, we use the list time instead.
        this->NeedUpdate=this->LastRenderTime<l->GetMTime();
        l=lights->GetNextItem();
        }
      }
    if(!this->NeedUpdate)
      {
      this->NeedUpdate=this->LastRenderTime<r->GetViewProps()->GetMTime()
        || this->LastRenderTime<latestPropTime;
      }

    if(!this->NeedUpdate)
      {
      int i=0;
      while(i<propArrayCount)
        {
        this->NeedUpdate=this->LastRenderTime<propArray[i]->GetMTime();
       ++i;
        }
      }
    size_t lightIndex=0;
    bool autoLight=r->GetAutomaticLightCreation()==1;
    vtkCamera *realCamera=r->GetActiveCamera();
    vtkRenderState s2(r);
    if(this->NeedUpdate) // create or re-create the shadow maps.
      {
#ifdef VTK_SHADOW_MAP_BAKER_PASS_DEBUG
      cout << "update the shadow maps" << endl;
#endif
      GLint savedDrawBuffer;
      glGetIntegerv(GL_DRAW_BUFFER,&savedDrawBuffer);

      realCamera->Register(this);

      // 1. Create a new render state with an FBO.

      // We need all the visible props, including the one culled out by the
      // camera,  because they can cast shadows too (ie being visible from the
      // light cameras)
      s2.SetPropArrayAndCount(propArray,propArrayCount);

      if(this->FrameBufferObject==0)
        {
        this->FrameBufferObject=vtkFrameBufferObject::New();
        this->FrameBufferObject->SetContext(context);
        }
      s2.SetFrameBuffer(this->FrameBufferObject);
      requiredKeys->Remove(vtkShadowMapBakerPass::RECEIVER());
      requiredKeys->Set(vtkShadowMapBakerPass::OCCLUDER(),0);
      s2.SetRequiredKeys(requiredKeys);

      lights->InitTraversal();
      l=lights->GetNextItem();
      size_t numberOfShadowLights=0;
      while(l!=0)
        {
        if(l->GetSwitch() && this->LightCreatesShadow(l))
          {
          ++numberOfShadowLights;
          }
        l=lights->GetNextItem();
        }

      if(this->ShadowMaps!=0 &&
         this->ShadowMaps->Vector.size()!=numberOfShadowLights)
        {
        delete this->ShadowMaps;
        this->ShadowMaps=0;
        }

      if(this->ShadowMaps==0)
        {
        this->ShadowMaps=new vtkShadowMapBakerPassTextures;
        this->ShadowMaps->Vector.resize(numberOfShadowLights);
        }

      if(this->LightCameras!=0 &&
         this->LightCameras->Vector.size()!=numberOfShadowLights)
        {
        delete this->LightCameras;
        this->LightCameras=0;
        }

      if(this->LightCameras==0)
        {
        this->LightCameras=new vtkShadowMapBakerPassLightCameras;
        this->LightCameras->Vector.resize(numberOfShadowLights);
        }

      r->SetAutomaticLightCreation(false);

      r->UpdateLightsGeometryToFollowCamera();
      double bb[6];
      vtkMath::UninitializeBounds(bb);
      vtkPropCollection* props = r->GetViewProps();
      vtkCollectionSimpleIterator cookie;
      props->InitTraversal(cookie);
      vtkProp* prop;
      bool first = true;
      while((prop = props->GetNextProp(cookie)) != NULL)
        {
        double* bounds = prop->GetBounds();
        if(first)
          {
          bb[0] = bounds[0];
          bb[1] = bounds[1];
          bb[2] = bounds[2];
          bb[3] = bounds[3];
          bb[4] = bounds[4];
          bb[5] = bounds[5];
          }
        else
          {
          bb[0] = (bb[0] < bounds[0] ? bb[0] : bounds[0]);
          bb[1] = (bb[1] > bounds[1] ? bb[1] : bounds[1]);
          bb[2] = (bb[2] < bounds[2] ? bb[2] : bounds[2]);
          bb[3] = (bb[3] > bounds[3] ? bb[3] : bounds[3]);
          bb[4] = (bb[4] < bounds[4] ? bb[4] : bounds[4]);
          bb[5] = (bb[5] > bounds[5] ? bb[5] : bounds[5]);
          }
        first = false;
        }

      lights->InitTraversal();
      l=lights->GetNextItem();
      lightIndex=0;
      while(l!=0)
        {
        if(l->GetSwitch() && this->LightCreatesShadow(l))
          {
          vtkTextureObject *map=this->ShadowMaps->Vector[lightIndex];
          if(map==0)
            {
            map=vtkTextureObject::New();
            this->ShadowMaps->Vector[lightIndex]=map;
            map->Delete();
            }

          map->SetContext(context);
          map->SetMinificationFilter(vtkTextureObject::Nearest);
          map->SetLinearMagnification(false);
          map->SetWrapS(vtkTextureObject::ClampToEdge);
          map->SetWrapT(vtkTextureObject::ClampToEdge);
          map->SetWrapR(vtkTextureObject::ClampToEdge);
          if(map->GetWidth()!=this->Resolution ||
             map->GetHeight()!=this->Resolution)
            {
            map->Create2D(this->Resolution,this->Resolution,
                          1,VTK_VOID,false);
            }
          this->FrameBufferObject->SetDepthBufferNeeded(true);
          this->FrameBufferObject->SetDepthBuffer(map);
          this->FrameBufferObject->StartNonOrtho(
            static_cast<int>(this->Resolution),
            static_cast<int>(this->Resolution),false);


          vtkCamera *lightCamera=this->LightCameras->Vector[lightIndex];
          if(lightCamera==0)
            {
            lightCamera=vtkCamera::New();
            this->LightCameras->Vector[lightIndex]=lightCamera;
            lightCamera->Delete();
            }

          // Build light camera
          r->SetActiveCamera(realCamera);


          this->BuildCameraLight(l,bb, lightCamera);
          r->SetActiveCamera(lightCamera);

          glShadeModel(GL_FLAT);
          glDisable(GL_LIGHTING);
          glDisable(GL_COLOR_MATERIAL);
          glDisable(GL_NORMALIZE);
          glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);

          glEnable(GL_POLYGON_OFFSET_FILL);
          glPolygonOffset(this->PolygonOffsetFactor,this->PolygonOffsetUnits);

          glEnable(GL_DEPTH_TEST);
          this->OpaquePass->Render(&s2);

          this->NumberOfRenderedProps+=
            this->OpaquePass->GetNumberOfRenderedProps();

          if(this->CompositeZPass!=0)
            {
            this->CompositeZPass->Render(&s2);
            }

          r->SetActiveCamera(realCamera); //reset the camera

#ifdef VTK_SHADOW_MAP_BAKER_PASS_DEBUG
          cout << "finish1 lightIndex=" <<lightIndex << endl;
          glFinish();

          state->Update();
          vtkIndent indent;

          std::ostringstream ost00;
          ost00.setf(ios::fixed,ios::floatfield);
          ost00.precision(5);
          ost00 << "OpenGLState_" << pthread_self() << "_"
                << vtkTimerLog::GetUniversalTime() << "_.txt";
          ofstream outfile(ost00.str().c_str());
          state->PrintSelf(outfile,indent);
          outfile.close();
#endif

#ifdef VTK_SHADOW_MAP_BAKER_PASS_DEBUG
          state->Update();
          std::ostringstream ost01;
          ost01.setf(ios::fixed,ios::floatfield);
          ost01.precision(5);
          ost01 << "OpenGLState_" << pthread_self() << "_"
                << vtkTimerLog::GetUniversalTime() << "_after_compositez.txt";
          ofstream outfile1(ost01.str().c_str());
          state->PrintSelf(outfile1,indent);
          outfile1.close();
#endif

          ++lightIndex;
          }
        l=lights->GetNextItem();
        }
      this->LastRenderTime.Modified(); // was a BUG

      glDisable(GL_POLYGON_OFFSET_FILL);
      glPolygonOffset(0.0f,0.0f);

      // back to the original frame buffer.
      this->FrameBufferObject->UnBind();
      glDrawBuffer(static_cast<GLenum>(savedDrawBuffer));

      // Restore real camera.
      r->SetActiveCamera(realCamera);
      realCamera->UnRegister(this);

      glShadeModel(GL_SMOOTH);
      glEnable(GL_LIGHTING);
      glEnable(GL_COLOR_MATERIAL);
      glEnable(GL_NORMALIZE);
      glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LEQUAL);

      r->SetAutomaticLightCreation(autoLight);

      } // end of the shadow map creations.
    requiredKeys->Delete();
    delete[] propArray;

    if (saved_scissor_test)
      {
      glEnable(GL_SCISSOR_TEST);
      }
    }
  else
    {
    vtkWarningMacro(<<" no delegate.");
    }

  vtkOpenGLCheckErrorMacro("failed after Render");
}

// ----------------------------------------------------------------------------
// Description:
// Build a camera from spot light parameters.
// \pre light_exists: light!=0
// \pre light_is_spotlight: light->LightTypeIsSceneLight() && light->GetPositional() && light->GetConeAngle()<180.0
// \pre camera_exists: camera!=0
void vtkShadowMapBakerPass::BuildCameraLight(vtkLight *light,
                                        double *bb,
                                        vtkCamera* lcamera)
{
  assert("pre: light_exists" && light!=0);
  assert("pre: camera_exists" && lcamera!=0);

  lcamera->SetPosition(light->GetTransformedPosition());
  lcamera->SetFocalPoint(light->GetTransformedFocalPoint());

  double dir[3];
  dir[0] = lcamera->GetFocalPoint()[0]-lcamera->GetPosition()[0];
  dir[1] = lcamera->GetFocalPoint()[1]-lcamera->GetPosition()[1];
  dir[2] = lcamera->GetFocalPoint()[2]-lcamera->GetPosition()[2];
  vtkMath::Normalize(dir);
  double vx[3], vup[3];
  vtkMath::Perpendiculars(dir, vx, vup, 0);
  double mNear, mFar;
  BoxNearFar(bb, lcamera->GetPosition(), dir, mNear, mFar);
  lcamera->SetViewUp(vup);

  if(light->GetPositional())
    {
    assert("pre: cone_angle_is_inf_180" && light->GetConeAngle()<180.0);

    lcamera->SetParallelProjection(0);
    // view angle is an aperture, but cone (or light) angle is between
    // the axis of the cone and a ray along the edge  of the cone.
    lcamera->SetViewAngle(light->GetConeAngle()*2.0);
    // initial clip=(0.1,1000). mNear>0, mFar>mNear);
    double mNearmin = (mFar - mNear) / 100.0;
    if(mNear < mNearmin)
      mNear = mNearmin;
    if(mFar < mNearmin)
      mFar = 2.0*mNearmin;
    lcamera->SetClippingRange(mNear,mFar);
    }
  else
    {
    lcamera->SetParallelProjection(1);

    double minx, maxx, miny, maxy, minz, maxz;
    double orig[3] = {0, 0, 0};
    this->BoxNearFar(bb, orig, vx, minx, maxx);
    this->BoxNearFar(bb, orig, vup, miny, maxy);
    this->BoxNearFar(bb, orig, dir, minz, maxz);

    double sizex, sizey;
    sizex = maxx-minx;
    sizey = maxy-miny;

    double realPos[3];
    realPos[0] = dir[0] * (minz - 1.0) + (minx+maxx) / 2.0 * vx[0] + (miny+maxy) / 2.0 * vup[0];
    realPos[1] = dir[1] * (minz - 1.0) + (minx+maxx) / 2.0 * vx[1] + (miny+maxy) / 2.0 * vup[1];
    realPos[2] = dir[2] * (minz - 1.0) + (minx+maxx) / 2.0 * vx[2] + (miny+maxy) / 2.0 * vup[2];

    lcamera->SetPosition(realPos);
    lcamera->SetFocalPoint(realPos[0] + dir[0], realPos[1] + dir[1], realPos[2] + dir[2]);
    double scale = (sizex > sizey ? sizex: sizey);
    lcamera->SetParallelScale(scale);
    lcamera->SetClippingRange(1.0, 1.0 + maxz - minz);

    }
}

// ----------------------------------------------------------------------------
// Description:
// Release graphics resources and ask components to release their own
// resources.
// \pre w_exists: w!=0
void vtkShadowMapBakerPass::ReleaseGraphicsResources(vtkWindow *w)
{
  assert("pre: w_exists" && w!=0);
  if(this->OpaquePass!=0)
    {
    this->OpaquePass->ReleaseGraphicsResources(w);
    }

  if(this->CompositeZPass!=0)
    {
    this->CompositeZPass->ReleaseGraphicsResources(w);
    }

  if(this->FrameBufferObject!=0)
    {
    this->FrameBufferObject->Delete();
    this->FrameBufferObject=0;
    }

  delete this->ShadowMaps;
  this->ShadowMaps=0;

  delete this->LightCameras;
  this->LightCameras=0;
}
