#include "ModularSynth.h"
#include "IAudioSource.h"
#include "IAudioProcessor.h"
#include "SynthGlobals.h"
#include "MidiInstrument.h"
#include "Scale.h"
#include "Transport.h"
#include "TextEntry.h"
#include "InputChannel.h"
#include "OutputChannel.h"
#include "TitleBar.h"
#include "LFOController.h"
#include "MidiController.h"
#include "ChaosEngine.h"
#include "ModuleSaveDataPanel.h"
#include "Profiler.h"
#include "MultitrackRecorder.h"
#include "Sample.h"
#include "FloatSliderLFOControl.h"
#include "VinylTempoControl.h"
#include "FreeverbOutput.h"
//#include <CoreServices/CoreServices.h>
#include "fenv.h"
#include <stdlib.h>
#include "GridController.h"
#include "PerformanceTimer.h"
#include "FileStream.h"
#include "PatchCable.h"
#include "ADSRDisplay.h"
#include "../JuceLibraryCode/JuceHeader.h"

ModularSynth* TheSynth = NULL;

#define RECORDING_LENGTH (gSampleRate*60*30) //30 minutes of recording

void AtExit()
{
   TheSynth->Exit();
}

ModularSynth::ModularSynth()
: mMoveModule(NULL)
, mOutputBufferLeft(RECORDING_LENGTH)
, mOutputBufferRight(RECORDING_LENGTH)
, mOutputBufferMeasurePos(RECORDING_LENGTH)
, mAudioPaused(false)
, mClickStartX(INT_MAX)
, mClickStartY(INT_MAX)
, mHeldSample(NULL)
, mConsoleListener(NULL)
, mLastClickWasOnModule(false)
, mInitialized(false)
, mRecordingLength(0)
, mGroupSelecting(false)
, mResizeModule(NULL)
, mShowLoadStatePopup(false)
, mHasDuplicatedDuringDrag(false)
, mFrameRate(0)
{
   mConsoleText[0] = 0;
   assert(TheSynth == NULL);
   TheSynth = this;
   
   mSaveOutputBuffer = new float[RECORDING_LENGTH*2];
}

ModularSynth::~ModularSynth()
{
   DeleteAllModules();
   
   SetMemoryTrackingEnabled(false); //avoid crashes when the tracking lists themselves are deleted
   
   assert(TheSynth == this);
   TheSynth = NULL;
}

bool ModularSynth::IsReady()
{
   return gTime > 100;
}

void ModularSynth::Setup(GlobalManagers* globalManagers, juce::Component* mainComponent)
{
   mGlobalManagers = globalManagers;
   mMainComponent = mainComponent;
   
   bool loaded = mUserPrefs.open(ofToDataPath("userprefs.json"));
   if (loaded)
   {
      SetGlobalBufferSize(mUserPrefs["buffersize"].asInt());
      mIOBufferSize = gBufferSize;
      gSampleRate = mUserPrefs["samplerate"].asInt();
      int width = mUserPrefs["width"].asInt();
      int height = mUserPrefs["height"].asInt();
      if (gIsRetina)
      {
         width *= 2;
         height *= 2;
      }
      if (width > 1 && height > 1)
      {
         //TODO_PORT(Ryan) this locks up in windows
#ifndef JUCE_WINDOWS
         MessageManagerLock lock;
         mainComponent->setSize(width, height);
#endif
      }
   }
   else
   {
      LogEvent("Couldn't find or load userprefs.json", kLogEventType_Error);
      SetGlobalBufferSize(64);
      mIOBufferSize = gBufferSize;
      gSampleRate = 44100;
   }
   
   SynthInit();

   new Transport();
   new Scale();
   TheScale->CreateUIControls();
   TheTransport->CreateUIControls();

   TheScale->Init();
   TheTransport->Init();
   
   ResetLayout();
   
   mConsoleListener = new ConsoleListener();
   mConsoleEntry = new TextEntry(mConsoleListener,"console",0,20,50,mConsoleText);
}

void ModularSynth::LoadResources(void* nanoVG)
{
   gNanoVG = (NVGcontext*)nanoVG;
   LoadGlobalResources();
}

static int sFrameCount = 0;
void ModularSynth::Poll()
{
   if (!mInitialized && sFrameCount > 3) //let some frames render before blocking for a load
   {
      LoadLayout(ofToDataPath(mUserPrefs["layout"].asString()));
      mInitialized = true;
   }
   
   mZoomer.Update();
   for (int i=0; i<mModules.size(); ++i)
      mModules[i]->BasePoll();
   
   if (mShowLoadStatePopup)
   {
      mShowLoadStatePopup = false;
      LoadStatePopupImp();
   }
   
   ++sFrameCount;
}

void ModularSynth::DeleteAllModules()
{
   for (int i=0; i<mModules.size(); ++i)
   {
      IDrawableModule* module = mModules[i];
      if (module->IsSingleton() == false)
         delete module;
   }
   mModules.clear();
   
   for (int i=0; i<mDeletedModules.size(); ++i)
      delete mDeletedModules[i];
   mDeletedModules.clear();
   
   delete TheScale;
   TheScale = NULL;
   delete TheTransport;
   TheTransport = NULL;
   delete mConsoleListener;
   mConsoleListener = NULL;
}

bool SortPointsByY(ofVec2f a, ofVec2f b)
{
   return a.y < b.y;
}

void ModularSynth::ZoomView(float zoomAmount)
{
   float oldDrawScale = gDrawScale;
   gDrawScale *= 1 + zoomAmount;
   float minZoom = .1f;
   float maxZoom = 8;
   gDrawScale = ofClamp(gDrawScale,minZoom,maxZoom);
   zoomAmount = (gDrawScale - oldDrawScale) / oldDrawScale; //find actual adjusted amount
   ofVec2f zoomCenter = ofVec2f(GetMouseX(), GetMouseY()) + mDrawOffset;
   mDrawOffset -= zoomCenter * zoomAmount;
   mZoomer.CancelMovement();
}

void ModularSynth::Draw(void* vg)
{
   gNanoVG = (NVGcontext*)vg;
   
   //DrawText("fps: "+ofToString(ofGetFrameRate(),4)+" "+ofToString(ofGetWidth()*ofGetHeight()), 100, 100,50);
   //return;
   
   mDrawRect.set(-mDrawOffset.x, -mDrawOffset.y, ofGetWidth() / gDrawScale, ofGetHeight() / gDrawScale);
   
   DrawLissajous(&mOutputBufferLeft, 0, 0, ofGetWidth(), ofGetHeight(), .7f, 0, 0);
   
   if (gTime == 1)
   {
      string loading("Bespoke is initializing audio...");
      DrawText(loading,ofGetWidth()/2-GetStringWidth(loading,30)/2,ofGetHeight()/2-6, 30);
      return;
   }
   
   if (!mInitialized)
   {
      string loading("Bespoke is loading...");
      DrawText(loading,ofGetWidth()/2-GetStringWidth(loading,30)/2,ofGetHeight()/2-6, 30);
      return;
   }
   
   ofPushMatrix();
   
   ofScale(gDrawScale,gDrawScale,gDrawScale);
   
   ofPushMatrix();
   
   ofTranslate(mDrawOffset.x, mDrawOffset.y);

	ofNoFill();
   
   TheTitleBar->SetPosition(-TheSynth->GetDrawOffset().x, -TheSynth->GetDrawOffset().y);
   TheSaveDataPanel->SetShowing(TheSaveDataPanel->GetModule());
   TheSaveDataPanel->UpdatePosition();
   
   for (int i = mModules.size()-1; i >= 0; --i)
   {
      if (!mModules[i]->AlwaysOnTop())
         mModules[i]->Draw();
   }
   
   for (int i = mModules.size()-1; i >= 0; --i)
   {
      if (mModules[i]->AlwaysOnTop())
         mModules[i]->Draw();
   }
   
   for (auto* modal : mModalFocusItemStack)
      modal->Draw();
   
   for (int i=0; i<mLissajousDrawers.size(); ++i)
   {
      int moduleX, moduleY;
      mLissajousDrawers[i]->GetPosition(moduleX, moduleY);
      IAudioSource* source = dynamic_cast<IAudioSource*>(mLissajousDrawers[i]);
      DrawLissajous(source->GetVizBuffer(), moduleX, moduleY-240, 240, 240);
   }
   
   if (mGroupSelecting)
   {
      ofPushStyle();
      ofSetColor(255,255,255);
      ofRect(mClickStartX, mClickStartY, GetMouseX()-mClickStartX, GetMouseY()-mClickStartY);
      ofPopStyle();
   }
   
   /*TODO_PORT(Ryan)
   const int starvationDisplayTicks = 500;
   if (mSoundStream.getTickCount() - mSoundStream.GetLastStarvationTick() < starvationDisplayTicks)
   {
      ofPushStyle();
      ofSetColor(255,255,255, (1.0f-(float(mSoundStream.getTickCount() - mSoundStream.GetLastStarvationTick())/starvationDisplayTicks))*255);
      DrawText("X", 5, 15);
      ofPopStyle();
   }*/

   if (mHeldSample)
   {
      ofPushMatrix();
      ofTranslate(GetMouseX(), GetMouseY());
      DrawAudioBuffer(100, 70, mHeldSample->Data(), 0, mHeldSample->LengthInSamples(), -1);
      ofPopMatrix();
   }
   
   ofPushStyle();
   ofNoFill();
   ofSetLineWidth(3);
   ofSetColor(0,255,0,100);
   ofSetCircleResolution(100);
   ofCircle(GetMouseX(), GetMouseY(), 30 + (TheTransport->GetMeasurePos() * 20));
   ofPopStyle();
   
   ofPopMatrix();
   
   Profiler::Draw();
   
   DrawConsole();
   
   ofPopMatrix();
}

void ModularSynth::DrawConsole()
{
   if (!mErrors.empty())
   {
      ofPushStyle();
      ofFill();
      ofSetColor(255,0,0,128);
      ofBeginShape();
      ofVertex(0,0);
      ofVertex(20,0);
      ofVertex(0,20);
      ofEndShape();
      ofPopStyle();
   }
   
   float consoleY = 51;
   
   if (TextEntry::GetActiveTextEntry() == mConsoleEntry)
   {
      mConsoleEntry->SetPosition(0, consoleY-15);
      mConsoleEntry->Draw();
      consoleY += 15;
   }
   
   int outputLines = mEvents.size();
   if (TextEntry::GetActiveTextEntry() == mConsoleEntry)
      outputLines += mErrors.size();
   if (outputLines > 0)
   {
      ofPushStyle();
      ofSetColor(0,0,0,150);
      ofFill();
      ofRect(10,consoleY-15,400,outputLines*15+3);
      ofPopStyle();
   }
   
   for (auto it=mEvents.begin(); it != mEvents.end();)
   {
      if (it->second + 3000 > gTime)
      {
         ofPushStyle();
         ofSetColor(255,255,0);
         DrawText(it->first, 10, consoleY);
         ofPopStyle();
         consoleY += 15;
         ++it;
      }
      else
      {
         it = mEvents.erase(it);
      }
   }
   
   if (TextEntry::GetActiveTextEntry() == mConsoleEntry)
   {
      if (!mErrors.empty())
      {
         ofPushStyle();
         ofSetColor(255,0,0);
         for (int i=0;i<mErrors.size();++i)
         {
            DrawText(mErrors[i], 10, consoleY);
            consoleY += 15;
         }
         ofPopStyle();
      }
   }
}

void ModularSynth::Exit()
{
   mAudioThreadMutex.Lock("exiting");
   mAudioPaused = true;
   mAudioThreadMutex.Unlock();
   mSoundStream.stop();
   for (int i=0; i<mModules.size(); ++i)
   {
      mModules[i]->Exit();
   }
   DeleteAllModules();
   ofExit();
}

IDrawableModule* ModularSynth::GetLastClickedModule() const
{
   if (mLastClickWasOnModule)
      return mModules[0];
   return NULL;
}

void ModularSynth::KeyPressed(int key)
{
   if (gHoveredUIControl &&
       TextEntry::GetActiveTextEntry() == NULL &&
       GetKeyModifiers() == kModifier_None &&
       (isdigit(key) || key == '.' || key == '-'))
   {
      gHoveredUIControl->AttemptTextInput();
   }
   
   if (TextEntry::GetActiveTextEntry())  //active text entry captures all input
   {
      TextEntry::GetActiveTextEntry()->OnKeyPressed(key);
      return;
   }
   
   key = KeyToLower(key);  //now convert to lowercase because everything else just cares about keys as buttons (unmodified by shift)
   
   if (key == OF_KEY_BACKSPACE)
   {
      for (auto module : mGroupSelectedModules)
         DeleteModule(module);
      mGroupSelectedModules.clear();
   }
   
   if (key == '`')
      ADSRDisplay::ToggleDisplayMode();

   if (key == 9)  //tab
   {
      bzero(mConsoleText, MAX_TEXTENTRY_LENGTH);
      mConsoleEntry->MakeActiveTextEntry();
   }
   
   mZoomer.OnKeyPressed(key);
   
   if (isdigit(key) && GetKeyModifiers() & kModifier_Command)
   {
      int num = key - '0';
      assert(num >= 0 && num <= 9);
      gHotBindUIControl[num] = gHoveredUIControl;
   }
   
   for (int i=0; i<mModules.size(); ++i)
      mModules[i]->KeyPressed(key);

   if (key == '/')
      ofToggleFullscreen();
   
   if (key == 'p')
      mAudioPaused = !mAudioPaused;
   
   //if (key == 'c')
   //   mousePressed(GetMouseX(), GetMouseY(), 0);
   
   //if (key == '=')
   //   ZoomView(.1f);
   //if (key == '-')
   //   ZoomView(-.1f);
   
   if (gHoveredUIControl)
   {
      if (key == OF_KEY_DOWN || key == OF_KEY_UP)
      {
         float inc;
         if ((key == OF_KEY_DOWN && gHoveredUIControl->InvertScrollDirection() == false) ||
             (key == OF_KEY_UP   && gHoveredUIControl->InvertScrollDirection() == true))
            inc = -1;
         else
            inc = 1;
         if (GetKeyModifiers() & kModifier_Shift)
            inc *= .01f;
         gHoveredUIControl->Increment(inc);
      }
      if (key == '[')
         gHoveredUIControl->Halve();
      if (key == ']')
         gHoveredUIControl->Double();
      if (key == '\\')
         gHoveredUIControl->ResetToOriginal();
   }
}

void ModularSynth::KeyReleased(int key)
{
   key = KeyToLower(key);
   
   //if (key == 'c')
   //   mouseReleased(GetMouseX(), GetMouseY(), 0);
   
   for (int i=0; i<mModules.size(); ++i)
      mModules[i]->KeyReleased(key);
}

void ModularSynth::MouseMoved(int intX, int intY )
{
   mMousePos.x = intX;
   mMousePos.y = intY;
   
   if (ofGetKeyPressed(' '))
   {
      mDrawOffset += (ofVec2f(intX,intY) - mLastMoveMouseScreenPos) / gDrawScale;
      mZoomer.CancelMovement();
   }
   
   mLastMoveMouseScreenPos = ofVec2f(intX,intY);
   
   float x = GetMouseX();
   float y = GetMouseY();

   for (auto* modal : mModalFocusItemStack)
      modal->NotifyMouseMoved(x,y);

   if (mMoveModule)
   {
      mMoveModule->SetPosition(x + mMoveModuleOffsetX, y + mMoveModuleOffsetY);
      return;
   }

   for (int i=mModules.size()-1; i>=0; --i)  //run this backwards so that we can figure out the top hover control
      mModules[i]->NotifyMouseMoved(x,y);
   
   if (gHoveredUIControl)
   {
      int uiX,uiY;
      gHoveredUIControl->GetPosition(uiX, uiY);
      int w, h;
      gHoveredUIControl->GetDimensions(w, h);
      
      if (x < uiX - 10 || y < uiY - 10 || x > uiX + w + 10 || y > uiY + h + 10)
         gHoveredUIControl = NULL;
   }
}

void ModularSynth::MouseDragged(int intX, int intY, int button)
{
   mMousePos.x = intX;
   mMousePos.y = intY;
   
   float x = GetMouseX();
   float y = GetMouseY();
   
   ofVec2f drag = ofVec2f(x,y) - mLastMouseDragPos;
   mLastMouseDragPos = ofVec2f(x,y);

   if (GetMoveModule() && (abs(mClickStartX-x) >= 1 || abs(mClickStartY-y) >= 1))
   {
      mClickStartX = INT_MAX;  //moved enough from click spot to reset
      mClickStartY = INT_MAX;
   }
   
   for (auto* modal : mModalFocusItemStack)
      modal->NotifyMouseMoved(x,y);
   
   if (GetKeyModifiers() == kModifier_Shift && !mHasDuplicatedDuringDrag)
   {
      vector<IDrawableModule*> newGroupSelectedModules;
      map<IDrawableModule*, IDrawableModule*> oldToNewModuleMap;
      for (auto module : mGroupSelectedModules)
      {
         if (!module->IsSingleton())
         {
            IDrawableModule* newModule = DuplicateModule(module);
            newGroupSelectedModules.push_back(newModule);
            oldToNewModuleMap[module] = newModule;
         }
      }
      for (auto module : newGroupSelectedModules)
      {
         for (auto* cableSource : module->GetPatchCableSources())
         {
            for (auto* cable : cableSource->GetPatchCables())
            {
               if (VectorContains(dynamic_cast<IDrawableModule*>(cable->GetTarget()), mGroupSelectedModules))
               {
                  cableSource->SetPatchCableTarget(cable, oldToNewModuleMap[dynamic_cast<IDrawableModule*>(cable->GetTarget())]);
               }
            }
         }
      }
      mGroupSelectedModules = newGroupSelectedModules;
      
      if (mMoveModule && !mMoveModule->IsSingleton())
         mMoveModule = DuplicateModule(mMoveModule);
      
      mHasDuplicatedDuringDrag = true;
   }
   
   for (auto module : mGroupSelectedModules)
   {
      int moduleX,moduleY;
      module->GetPosition(moduleX, moduleY);
      module->SetPosition(moduleX + drag.x, moduleY + drag.y);
   }

   if (mMoveModule)
   {
      mMoveModule->SetPosition(x + mMoveModuleOffsetX, y + mMoveModuleOffsetY);
      return;
   }
   
   if (mResizeModule)
   {
      int moduleX,moduleY;
      mResizeModule->GetPosition(moduleX, moduleY);
      mResizeModule->Resize(x - moduleX, y - moduleY);
   }

   for (int i=0; i<mModules.size(); ++i)
      mModules[i]->NotifyMouseMoved(x,y);
}

void ModularSynth::MousePressed(int intX, int intY, int button)
{
   mMousePos.x = intX;
   mMousePos.y = intY;
   
   float x = GetMouseX();
   float y = GetMouseY();
   
   mLastMouseDragPos = ofVec2f(x,y);
   mGroupSelecting = false;
   
   bool rightButton = button == 2;

   TextEntry::ClearActiveTextEntry(K(acceptEntry));

   if (GetTopModalFocusItem())
   {
      bool clicked = GetTopModalFocusItem()->TestClick(x,y,rightButton);
      if (!clicked)
      {
         FloatSliderLFOControl* lfo = dynamic_cast<FloatSliderLFOControl*>(GetTopModalFocusItem());
         if (lfo) //if it's an LFO, don't dismiss it if you're adjusting the slider
         {
            FloatSlider* slider = lfo->GetOwner();
            int uiX,uiY;
            slider->GetPosition(uiX, uiY);
            int w, h;
            slider->GetDimensions(w, h);
            
            if (x < uiX || y < uiY || x > uiX + w || y > uiY + h)
               PopModalFocusItem();
         }
         else  //otherwise, always dismiss if you click outside it
         {
            PopModalFocusItem();
         }
      }
      else
      {
         return;
      }
   }

   if (InMidiMapMode())
   {
      if (gBindToUIControl == gHoveredUIControl)   //if it's the same, clear it
         gBindToUIControl = NULL;
      else
         gBindToUIControl = gHoveredUIControl;
      return;
   }
   
   IDrawableModule* clicked = GetModuleAt(x,y);
   
   for (auto cable : mPatchCables)
   {
      if (clicked &&
          (clicked == GetTopModalFocusItem() ||
           clicked->AlwaysOnTop() ||
           GetModuleIndex(clicked) < GetModuleIndex(cable->GetOwningModule())))
         break;
      if (cable->TestClick(x,y,rightButton))
      {
         return;
      }
   }
   
   mClickStartX = x;
   mClickStartY = y;
   mGroupSelecting = (clicked == NULL);
   mLastClickWasOnModule = (clicked != NULL && clicked != TheTitleBar);
   mHasDuplicatedDuringDrag = false;
   
   if (mGroupSelectedModules.empty() == false)
   {
      if (!VectorContains(clicked, mGroupSelectedModules))
         mGroupSelectedModules.clear();
      return;
   }

   if (clicked)
      CheckClick(clicked, x, y, rightButton);
   else
      TheSaveDataPanel->SetModule(NULL);
}

void ModularSynth::MouseScrolled(float x, float y)
{
   if (ofGetKeyPressed(' '))
   {
      ZoomView(y/100);
   }
   else if (gHoveredUIControl)
   {
#if JUCE_WINDOWS
      y *= -1;
      y -= x / 3; //taking advantage of logitech horizontal scroll wheel
#endif

      float val = gHoveredUIControl->GetMidiValue();
      float movementScale = 3;
      FloatSlider* floatSlider = dynamic_cast<FloatSlider*>(gHoveredUIControl);
      IntSlider* intSlider = dynamic_cast<IntSlider*>(gHoveredUIControl);
      if (floatSlider || intSlider)
      {
         int w,h;
         gHoveredUIControl->GetDimensions(w, h);
         movementScale = 200.0f / w;
            
         if (GetKeyModifiers() & kModifier_Shift)
            movementScale *= .01f;
      }
         
      float change = -y/100 * movementScale;
         
      if (floatSlider && floatSlider->GetLFO() && floatSlider->GetLFO()->Enabled())
      {
         FloatSliderLFOControl* lfo = floatSlider->GetLFO();
         float min = floatSlider->GetMin();
         float max = floatSlider->GetMax();
         float lfoMin = ofMap(lfo->Min(),min,max,0,1);
         float lfoMax = ofMap(lfo->Max(),min,max,0,1);
            
         float changeX = x/100 * movementScale;
            
         lfo->SetMin(ofMap(lfoMin + change,0,1,min,max,K(clamp)));
         lfo->SetMax(ofMap(lfoMax + changeX,0,1,min,max,K(clamp)));
            
         return;
      }
         
      if (gHoveredUIControl->InvertScrollDirection())
         val -= change;
      else
         val += change;
      val = ofClamp(val, 0, 1);
      gHoveredUIControl->SetFromMidiCC(val);
   }
   else
   {
#if JUCE_WINDOWS
      y *= -1;
      y -= x / 3; //taking advantage of logitech horizontal scroll wheel
#endif

      IDrawableModule* module = GetModuleAt(GetMouseX(), GetMouseY());
      if (module)
         module->NotifyMouseScrolled(GetMouseX(), GetMouseY(), x, y);
   }
}

bool ModularSynth::InMidiMapMode()
{
   return IsKeyHeld('m');
}

bool ModularSynth::ShouldAccentuateActiveModules() const
{
   return IsKeyHeld('s');
}

void ModularSynth::RegisterPatchCable(PatchCable* cable)
{
   mPatchCables.push_back(cable);
}

void ModularSynth::UnregisterPatchCable(PatchCable* cable)
{
   RemoveFromVector(cable, mPatchCables);
}

void ModularSynth::PushModalFocusItem(IDrawableModule* item)
{
   mModalFocusItemStack.push_back(item);
}

void ModularSynth::PopModalFocusItem()
{
   if (mModalFocusItemStack.empty() == false)
      mModalFocusItemStack.pop_back();
}

IDrawableModule* ModularSynth::GetTopModalFocusItem() const
{
   if (mModalFocusItemStack.empty())
      return NULL;
   return mModalFocusItemStack.back();
}

IDrawableModule* ModularSynth::GetModuleAt(int x, int y)
{
   if (GetTopModalFocusItem() && GetTopModalFocusItem()->TestClick(x, y, false, true))
      return GetTopModalFocusItem();
   for (int i=0; i<mModules.size(); ++i)
   {
      if (mModules[i]->AlwaysOnTop() && mModules[i]->TestClick(x,y,false,true))
         return mModules[i];
   }
   for (int i=0; i<mModules.size(); ++i)
   {
      if (mModules[i]->AlwaysOnTop() == false && mModules[i]->TestClick(x,y,false,true))
         return mModules[i];
   }
   return NULL;
}

void ModularSynth::CheckClick(IDrawableModule* clickedModule, int x, int y, bool rightButton)
{
   if (clickedModule != TheTitleBar)
      MoveToFront(clickedModule);
   
   //check to see if we clicked in the move area
   int moduleX, moduleY;
   clickedModule->GetPosition(moduleX, moduleY);
   int modulePosY = y - moduleY;
   
   if (modulePosY < 0)
   {
      mMoveModule = clickedModule;
      mMoveModuleOffsetX = moduleX - x;
      mMoveModuleOffsetY = moduleY - y;
   }
   
   //do the regular click
   clickedModule->TestClick(x,y,rightButton);
}

void ModularSynth::MoveToFront(IDrawableModule* module)
{
   for (int i=0; i<mModules.size(); ++i)
   {
      if (mModules[i] == module)
      {
         for (int j=i; j>0; --j)
            mModules[j] = mModules[j-1];
         mModules[0] = module;
         
         break;
      }
   }
}

void ModularSynth::DeleteModule(IDrawableModule* module)
{
   if (module == TheScale ||
       module == TheTransport ||
       module == TheSaveDataPanel ||
       module == TheTitleBar)
      return;
   
   mAudioThreadMutex.Lock("delete");
   
   list<PatchCable*> cablesToRemove;
   for (auto* cable : mPatchCables)
   {
      if (cable->GetOwningModule() == module)
         cablesToRemove.push_back(cable);
   }
   for (auto* cable : cablesToRemove)
      RemoveFromVector(cable, mPatchCables);
   
   RemoveFromVector(module, mModules, K(fail));
   RemoveFromVector(dynamic_cast<MidiInstrument*>(module),mInstruments);
   RemoveFromVector(dynamic_cast<IAudioSource*>(module),mSources);
   RemoveFromVector(module,mLissajousDrawers);
   TheTransport->RemoveAudioPoller(dynamic_cast<IAudioPoller*>(module));
   module->SetEnabled(false);
   module->Exit();
   mDeletedModules.push_back(module);
   //delete module; TODO(Ryan) deleting is hard... need to clear out everything with a reference to this, or switch to smart pointers
   
   if (module == TheChaosEngine)
      TheChaosEngine = NULL;
   if (module == TheFreeverbOutput)
      TheFreeverbOutput = NULL;
   if (module == TheLFOController)
      TheLFOController = NULL;
   if (module == TheVinylTempoControl)
      TheVinylTempoControl = NULL;
   
   for (int i=0; i<MAX_INPUT_CHANNELS; ++i)
   {
      if (module == mInput[i])
         mInput[i] = NULL;
   }
   
   for (int i=0; i<MAX_OUTPUT_CHANNELS; ++i)
   {
      if (module == mOutput[i])
         mOutput[i] = NULL;
   }
   
   for (auto iter : mModules)
   {
      if (iter->GetPatchCableSource())
      {
         for (auto cable : iter->GetPatchCableSource()->GetPatchCables())
         {
            if (cable->GetTarget() == module)
               cable->Destroy();
         }
      }
   }
   
   mAudioThreadMutex.Unlock();
}

void ModularSynth::MouseReleased(int intX, int intY, int button)
{
   mMousePos.x = intX;
   mMousePos.y = intY;
   
   float x = GetMouseX();
   float y = GetMouseY();
   
   if (GetTopModalFocusItem())
   {
      GetTopModalFocusItem()->MouseReleased();
   }

   if (mMoveModule)
   {
      int moduleX, moduleY;
      mMoveModule->GetPosition(moduleX, moduleY);
      mMoveModule = NULL;
   }
   
   if (mResizeModule)
      mResizeModule = NULL;

   for (int i = 0; i < mModules.size(); i++)
      mModules[i]->MouseReleased();
   
   if (mHeldSample)
   {
      IDrawableModule* module = GetModuleAt(x, y);
      if (module)
      {
         int moduleX, moduleY;
         module->GetPosition(moduleX, moduleY);
         module->SampleDropped(x-moduleX, y-moduleY, GetHeldSample());
      }
      ClearHeldSample();
   }
   
   if (mGroupSelecting)
   {
      mGroupSelecting = false;
      mGroupSelectedModules.clear();
      for (int i=0; i<mModules.size(); ++i)
      {
         if (mModules[i]->IsWithinRect(ofRectangle(ofPoint(mClickStartX,mClickStartY),ofPoint(x,y))))
            mGroupSelectedModules.push_back(mModules[i]);
      }
      for (int i=mGroupSelectedModules.size()-1; i>=0; --i) //do this backwards to preserve existing order
         MoveToFront(mGroupSelectedModules[i]);
   }
   
   mClickStartX = INT_MAX;
   mClickStartY = INT_MAX;
}

void ModularSynth::AudioOut(float** output, int bufferSize, int nChannels)
{
   Profiler profiler("audioOut() total", true);
   
   if (mAudioPaused)
      return;
   
   ScopedMutex mutex(&mAudioThreadMutex, "audioOut()");
   
   assert(nChannels <= MAX_OUTPUT_CHANNELS);
   
   /////////// AUDIO PROCESSING STARTS HERE /////////////
   float* outBuffer[MAX_OUTPUT_CHANNELS];
   assert(bufferSize == mIOBufferSize);
   assert(mIOBufferSize == gBufferSize);  //need to be the same for now
                                          //if we want these different, need to fix outBuffer here, and also fix audioIn()
   for (int ioOffset = 0; ioOffset < mIOBufferSize; ioOffset += gBufferSize)
   {
      int blah;
      if (TheVinylTempoControl &&
          mInput[TheVinylTempoControl->GetLeftChannel()-1] &&
          mInput[TheVinylTempoControl->GetRightChannel()-1])
      {
         TheVinylTempoControl->SetVinylControlInput(
               mInput[TheVinylTempoControl->GetLeftChannel()-1]->GetBuffer(blah),
               mInput[TheVinylTempoControl->GetRightChannel()-1]->GetBuffer(blah), gBufferSize);
      }

      for (int i=0; i<nChannels; ++i)
      {
         if (mOutput[i])
            mOutput[i]->ClearBuffer();
      }
      
      //get audio from sources
      for (int i=0; i<mSources.size(); ++i)
         mSources[i]->Process(gTime);
      
      //put it into speakers
      for (int i=0; i<MAX_OUTPUT_CHANNELS; ++i)
         outBuffer[i] = gZeroBuffer;
      int outBufferSize = gBufferSize;
      for (int i=0; i<nChannels; ++i)
      {
         if (mOutput[i])
         {
            outBuffer[i] = mOutput[i]->GetBuffer(outBufferSize);
            assert(outBufferSize == gBufferSize);
         }
      }
      
      if (TheFreeverbOutput)
      {
         TheFreeverbOutput->ProcessAudio(outBuffer[TheFreeverbOutput->GetLeftChannel()-1], outBuffer[TheFreeverbOutput->GetRightChannel()-1], outBufferSize);
      }
      
      if (TheMultitrackRecorder)
         TheMultitrackRecorder->Process(gTime, outBuffer[0], outBuffer[1], outBufferSize);
      
      for (int ch=0; ch<nChannels; ++ch)
         memcpy(output[ch]+ioOffset, outBuffer[ch]+ioOffset, gBufferSize*sizeof(float));
      
      double elapsed = gInvSampleRateMs * gBufferSize;
      gTime += elapsed;
      TheTransport->Advance((float)elapsed);
   }
   /////////// AUDIO PROCESSING ENDS HERE /////////////

   for (int i=0; i<bufferSize; ++i)
      mOutputBufferMeasurePos.Write(TheTransport->GetMeasurePos(i));
   
   mOutputBufferLeft.WriteChunk(outBuffer[0], bufferSize);
   mOutputBufferRight.WriteChunk(outBuffer[1], bufferSize);
   mRecordingLength += bufferSize;
   mRecordingLength = MIN(mRecordingLength, RECORDING_LENGTH);
   
   Profiler::PrintCounters();
}

void ModularSynth::AudioIn(const float** input, int bufferSize, int nChannels)
{
   if (mAudioPaused)
      return;
   
   ScopedMutex mutex(&mAudioThreadMutex, "audioIn()");

   assert(bufferSize == mIOBufferSize);
   assert(nChannels <= MAX_INPUT_CHANNELS);
   
   int inBufferSize;
   for (int i=0; i<nChannels; ++i)
   {
      if (mInput[i])
      {
         memcpy(mInput[i]->GetBuffer(inBufferSize), input[i], sizeof(float)*bufferSize);
         assert(inBufferSize == gBufferSize);
      }
   }
}

void ModularSynth::FilesDropped(vector<string> files, int intX, int intY)
{
   if (files.size() > 0)
   {
      float x = GetMouseX(intX);
      float y = GetMouseY(intY);
      for (int i = 0; i < mModules.size(); i++)
      {
         if (mModules[i]->TestClick(x,y,false,true))
         {
            int moduleX, moduleY;
            mModules[i]->GetPosition(moduleX, moduleY);
            x -= moduleX;
            y -= moduleY;
            mModules[i]->FilesDropped(files, x, y);
            break;
         }
      }
   }
}

struct SourceDepInfo
{
   SourceDepInfo(IAudioSource* me) : mMe(me) {}
   IAudioSource* mMe;
   vector<IAudioSource*> mDeps;
};

void ModularSynth::ArrangeAudioSourceDependencies()
{
   //ofLog() << "Calculating audio source dependencies:";
   
   vector<SourceDepInfo> deps;
   for (int i=0; i<mSources.size(); ++i)
      deps.push_back(SourceDepInfo(mSources[i]));
      
   for (int i=0; i<mSources.size(); ++i)
   {
      for (int j=0; j<mSources.size(); ++j)
      {
         if (mSources[i]->GetTarget() != NULL &&
             mSources[i]->GetTarget() == dynamic_cast<IAudioReceiver*>(mSources[j]))
         {
            deps[j].mDeps.push_back(mSources[i]);
         }

         //second stereo channel
         if (mSources[i]->GetTarget2() &&
             mSources[i]->GetTarget2() == dynamic_cast<IAudioReceiver*>(mSources[j]))
         {
            deps[j].mDeps.push_back(mSources[i]);
         }
      }
   }
   
   for (int i=0; i<deps.size(); ++i)
   {
      string depStr;
      for (int j=0;j<deps[i].mDeps.size();++j)
      {
         depStr += dynamic_cast<IDrawableModule*>(deps[i].mDeps[j])->Name();
         if (j<deps[i].mDeps.size()-1)
            depStr += ", ";
      }
      //ofLog() << dynamic_cast<IDrawableModule*>(deps[i].mMe)->Name() << "depends on:" << depStr;
   }
   
   //TODO(Ryan) detect circular dependencies
   
   mSources.clear();
   int loopCount = 0;
   while (deps.size() > 0 && loopCount < 1000) //stupid circular dependency detection, make better
   {
      for (int i=0; i<deps.size(); ++i)
      {
         bool hasDeps = false;
         for (int j=0; j<deps[i].mDeps.size(); ++j)
         {
            bool found = false;
            for (int k=0; k<mSources.size(); ++k)
            {
               if (deps[i].mDeps[j] == mSources[k])
                  found = true;
            }
            if (!found) //has a dep that hasn't been added yet
               hasDeps = true;
         }
         if (!hasDeps)
         {
            mSources.push_back(deps[i].mMe);
            deps.erase(deps.begin() + i);
            i-=1;
         }
      }
      ++loopCount;
   }
   
   if (loopCount == 1000)  //circular dependency, don't lose the rest of the sources
   {
      ofLog() << "circular dependency detected";
      for (int i=0; i<deps.size(); ++i)
         mSources.push_back(deps[i].mMe);
   }
   
   /*ofLog() << "new ordering:";
   for (int i=0; i<mSources.size(); ++i)
      ofLog() << dynamic_cast<IDrawableModule*>(mSources[i])->Name();*/
}

void ModularSynth::ResetLayout()
{
   for (int i=0; i<mModules.size(); ++i)
   {
      IDrawableModule* module = mModules[i];
      if (module->IsSingleton() == false)
         module->Exit();
   }
   
   for (int i=0; i<mModules.size(); ++i)
   {
      IDrawableModule* module = mModules[i];
      if (module->IsSingleton() == false &&
          dynamic_cast<FloatSliderLFOControl*>(module) == NULL)
      {
         delete module;
      }
   }
   
   for (int i=0; i<mDeletedModules.size(); ++i)
      delete mDeletedModules[i];
   
   for (int i=0; i<MAX_INPUT_CHANNELS; ++i)
      mInput[i] = NULL;
   for (int i=0; i<MAX_OUTPUT_CHANNELS; ++i)
      mOutput[i] = NULL;

   mModules.clear();
   mDeletedModules.clear();
   mInstruments.clear();
   mSources.clear();
   mLissajousDrawers.clear();
   mMoveModule = NULL;
   LFOPool::Shutdown();
   TextEntry::ClearActiveTextEntry(!K(acceptEntry));
   
   mEvents.clear();
   mErrors.clear();
   
   for (auto cable : mPatchCables)
      delete cable;
   mPatchCables.clear();
   
   gBindToUIControl = NULL;
   mModalFocusItemStack.clear();
   gHoveredUIControl = NULL;
   mLastClickWasOnModule = false;

   LFOPool::Init();
   
   TitleBar* titleBar = new TitleBar();
   titleBar->SetPosition(0,0);
   titleBar->SetName("titlebar");
   titleBar->CreateUIControls();
   titleBar->SetModuleFactory(&mModuleFactory);
   mModules.push_back(titleBar);
   
   ModuleSaveDataPanel* saveDataPanel = new ModuleSaveDataPanel();
   saveDataPanel->SetPosition(-200, 50);
   saveDataPanel->SetName("savepanel");
   saveDataPanel->CreateUIControls();
   mModules.push_back(saveDataPanel);
   
   if (gIsRetina)
      gDrawScale = 2.0f;
   mDrawOffset.set(0,0);
   mZoomer.Init();
}

bool ModularSynth::SetInputChannel(int channel, InputChannel* input)
{
   assert(channel > 0 && channel <= MAX_INPUT_CHANNELS);
   
   //ScopedMutex mutex(&mAudioThreadMutex, "SetInputChannel()");
   
   for (int i=0; i<MAX_INPUT_CHANNELS; ++i)  //remove if we're changing an already assigned channel
   {
      if (mInput[channel-1] == input)
         mInput[channel-1] = NULL;
   }
   
   if (mInput[channel-1] == NULL)
   {
      mInput[channel-1] = input;
      return true;
   }
   
   return false;
}

bool ModularSynth::SetOutputChannel(int channel, OutputChannel* output)
{
   assert(channel > 0 && channel <= MAX_OUTPUT_CHANNELS);
   
   //ScopedMutex mutex(&mAudioThreadMutex, "SetOutputChannel()");
   if (mOutput[channel-1] == NULL)
   {
      mOutput[channel-1] = output;
      return true;
   }
   
   return false;
}

void ModularSynth::LoadLayout(string jsonFile, bool makeDefaultLayout /*= true*/)
{
   //ofLoadURLAsync("http://bespoke.com/telemetry/"+jsonFile);
   
   ScopedMutex mutex(&mAudioThreadMutex, "LoadLayout()");
   ScopedLock renderLock(mRenderLock);
   
   ofLog() << "Loading layout: " << jsonFile;
   
   mLoadedLayoutPath = String(jsonFile).replace(ofToDataPath("").c_str(), "").toStdString();
   
   ResetLayout();
   
   ofxJSONElement root;
   bool loaded = root.open(jsonFile);
   
   if (!loaded)
   {
      LogEvent("Couldn't load, error parsing "+jsonFile, kLogEventType_Error);
      LogEvent("Try loading it up in a json validator", kLogEventType_Error);
      return;
   }
   
   PerformanceTimer timer;
   {
      TimerInstance t("load", timer);

      //two-pass loading for dependencies
      ofxJSONElement readModules = root["modules"];
      
      {
         TimerInstance t("create", timer);
         for (int i=0; i<readModules.size(); ++i)
         {
            try
            {
               TimerInstance t("create "+readModules[i]["name"].asString(), timer);
               CreateModule(readModules[i]);
            }
            catch (LoadingJSONException& e)
            {
               LogEvent("Couldn't load "+jsonFile, kLogEventType_Error);
            }
         }
      }

      {
         TimerInstance t("setup", timer);
         for (int i=0; i<readModules.size(); ++i)
         {
            try
            {
               TimerInstance t("setup "+readModules[i]["name"].asString(), timer);
               SetUpModule(readModules[i]);
            }
            catch (LoadingJSONException& e)
            {
               LogEvent("Couldn't set up "+jsonFile, kLogEventType_Error);
            }
            catch (UnknownModuleException& e)
            {
               LogEvent("Couldn't find module \""+e.mSearchName+"\"", kLogEventType_Error);
            }
         }
      }
      
      {
         TimerInstance t("init", timer);
         for (int i=0; i<mModules.size(); ++i)
         {
            TimerInstance t(string("init ")+mModules[i]->Name(), timer);
            if (mModules[i]->IsSingleton() == false)
               mModules[i]->Init();
         }
      }
   }
   
   //timer.PrintCosts();
   
   mZoomer.LoadFromSaveData(root["zoomlocations"]);
   ArrangeAudioSourceDependencies();
   
   if (makeDefaultLayout)
      UpdateUserPrefsLayout();
}

void ModularSynth::UpdateUserPrefsLayout()
{
   mUserPrefs["layout"] = mLoadedLayoutPath;
   mUserPrefs.save(ofToDataPath("userprefs.json"), true);
}

IDrawableModule* ModularSynth::CreateModule(const ofxJSONElement& moduleInfo)
{
   IDrawableModule* module = NULL;

   if (moduleInfo["comment_out"].asBool()) //hack since json doesn't allow comments
      return NULL;

   string type = moduleInfo["type"].asString();
   
   try
   {
      if (type == "transport")
      {
         module = TheTransport;
      }
      else if (type == "scale")
      {
         module = TheScale;
      }
      else
      {
         module = mModuleFactory.MakeModule(type);
      }
   
      if (module == NULL)
      {
         LogEvent("Couldn't create unknown module type \""+type+"\"", kLogEventType_Error);
         return NULL;
      }
      
      if (module->IsSingleton() == false)
         module->CreateUIControls();
      module->LoadBasics(moduleInfo, type);
      assert(strlen(module->Name()) > 0);
      
      mModules.resize(mModules.size() + 1);
      for (int i=mModules.size() - 1; i>0; --i)
      {
         mModules[i] = mModules[i-1];
      }
      mModules[0] = module;
   }
   catch (UnknownModuleException& e)
   {
      LogEvent("Couldn't find referenced module \""+e.mSearchName+"\" when loading \""+moduleInfo["name"].asString()+"\"", kLogEventType_Error);
   }
   
   return module;
}

IDrawableModule* ModularSynth::SetUpModule(const ofxJSONElement& moduleInfo)
{
   IDrawableModule* module = FindModule(moduleInfo["name"].asString());

   if (module == NULL)
   {
      LogEvent("Couldn't find module \""+moduleInfo["name"].asString()+"\" to set up", kLogEventType_Error);
      return NULL;
   }

   try
   {
      module->LoadLayout(moduleInfo);
      
      IAudioSource* source = dynamic_cast<IAudioSource*>(module);
      if (source)
         mSources.push_back(source);
      
      MidiInstrument* inst = dynamic_cast<MidiInstrument*>(module);
      if (inst)
         mInstruments.push_back(inst);
   }
   catch (UnknownModuleException& e)
   {
      LogEvent("Couldn't find referenced module \""+e.mSearchName+"\" when setting up \""+moduleInfo["name"].asString()+"\"", kLogEventType_Error);
   }
   
   return module;
}

void ModularSynth::AddModule(IDrawableModule* module)
{
   IAudioSource* source = dynamic_cast<IAudioSource*>(module);
   if (source)
      mSources.push_back(source);
   
   MidiInstrument* inst = dynamic_cast<MidiInstrument*>(module);
   if (inst)
      mInstruments.push_back(inst);
   
   mModules.push_back(module);
   MoveToFront(module);
}

int ModularSynth::GetModuleIndex(IDrawableModule* module)
{
   while (module->GetParent())
      module = dynamic_cast<IDrawableModule*>(module->GetParent());
   for (int i=0; i<mModules.size(); ++i)
   {
      if (module == mModules[i])
         return i;
   }
   assert(false);
   return -1;
}

IDrawableModule* ModularSynth::FindModule(string name, bool fail)
{
   if (name == "")
      return NULL;
   
   for (int i=0; i<mModules.size(); ++i)
   {
      if (name == mModules[i]->Name())
         return mModules[i];
   }
   
   if (fail)
      throw UnknownModuleException(name);
   return NULL;
}

MidiController* ModularSynth::FindMidiController(string name, bool fail)
{
   if (name == "")
      return NULL;
   
   MidiController* m = NULL;
   
   try
   {
      m = dynamic_cast<MidiController*>(FindModule(name, fail));
      if (m == NULL && fail)
         throw WrongModuleTypeException();
   }
   catch (UnknownModuleException& e)
   {
      LogEvent("Couldn't find referenced midi controller \""+name+"\"", kLogEventType_Error);
   }
   catch (WrongModuleTypeException& e)
   {
      LogEvent("\""+name+"\" is not a midicontroller", kLogEventType_Error);
   }
   
   return m;
}

IUIControl* ModularSynth::FindUIControl(string path)
{
   if (path == "")
      return NULL;
   
   vector<string> tokens = ofSplitString(path,"~");
   string control = tokens[tokens.size()-1];
   IDrawableModule* module = NULL;
   
   if (tokens.size() == 2)
   {
      module = FindModule(tokens[0], false);
   }
   else if (tokens.size() == 3)
   {
      for (int i=0; i<mModules.size(); ++i)
      {
         if (tokens[0] == mModules[i]->Name())
         {
            try
            {
               module = mModules[i]->FindChild(tokens[1].c_str());
            }
            catch (UnknownModuleException)
            {
            }
            break;
         }
      }
   }
   else
   {
      throw BadUIControlPathException();
   }

   if (module)
   {
      try
      {
         return module->FindUIControl(control.c_str());
      }
      catch (UnknownUIControlException& e)
      {
         LogEvent("Couldn't find UI control at path \""+path+"\"", kLogEventType_Error);
         return NULL;
      }
   }

   LogEvent("Couldn't find module in path \""+path+"\"", kLogEventType_Error);
   return NULL;
}

void ModularSynth::GrabSample(float* data, int length, bool window, int numBars)
{
   delete mHeldSample;
   mHeldSample = new Sample();
   mHeldSample->Create(data, length);
   mHeldSample->SetNumBars(numBars);
   
   //window sample to avoid clicks
   if (window)
   {
      const int fadeSamples = 15;
      if (length > fadeSamples * 2) //only window if there's enough space
      {
         for (int i=0; i<fadeSamples; ++i)
         {
            float fade = float(i)/fadeSamples;
            mHeldSample->Data()[i] *= fade;
            mHeldSample->Data()[length-1-i] *= fade;
         }
      }
   }
}

void ModularSynth::ClearHeldSample()
{
   delete mHeldSample;
   mHeldSample = NULL;
}

void ModularSynth::LogEvent(string event, LogEventType type)
{
   if (type == kLogEventType_Normal)
   {
      ofLog() << "event: " << event;
      
      mEvents.push_back(std::pair<string,double>(event,gTime));
   }
   else if (type == kLogEventType_Error)
   {
      ofLog() << "error: " << event;
      
      mErrors.push_back(event);
   }
}

IDrawableModule* ModularSynth::DuplicateModule(IDrawableModule* module)
{
   {
      FileStreamOut out("./tmp");
      module->SaveState(out);
   }
   
   ofxJSONElement layoutData;
   module->SaveLayout(layoutData);
   string newName = GetUniqueName(layoutData["name"].asString(), mModules);
   layoutData["name"] = newName;
   
   IDrawableModule* newModule = CreateModule(layoutData);
   SetUpModule(layoutData);
   newModule->Init();
   
   assert(newModule);
   
   newModule->SetName(module->Name()); //temporarily rename to the same as what we duplicated, so we can load state properly
   
   {
      FileStreamIn in("./tmp");
      newModule->LoadState(in);
   }
   
   newModule->SetName(newName.c_str());
   
   return newModule;
}

bool ModuleSorter(const IDrawableModule* a, const IDrawableModule* b)
{
   return string(a->Name()) < string(b->Name());
}

void ModularSynth::SaveLayout(string jsonFile, bool makeDefaultLayout /*= true*/)
{
   if (jsonFile.empty())
      jsonFile = ofToDataPath(mLoadedLayoutPath);
   
   for (auto i = mModules.begin(); i!= mModules.end(); ++i)
      UpdateTarget(*i);
   
   ofxJSONElement root;
   
   ofxJSONElement modules;
   
   vector<IDrawableModule*> saveModules;
   for (int i=0; i<mModules.size(); ++i)
   {
      IDrawableModule* module = mModules[i];
      if (module->IsSaveable())
         saveModules.push_back(module);
   }
   
   sort(saveModules.begin(), saveModules.end(), ModuleSorter);
   
   modules.resize(saveModules.size());
   
   for (int i=0; i<saveModules.size(); ++i)
   {
      ofxJSONElement moduleInfo;
      saveModules[i]->SaveLayout(moduleInfo);
      modules[i] = moduleInfo;
   }

   root["modules"] = modules;
   root["zoomlocations"] = mZoomer.GetSaveData();
   root.save(jsonFile, true);
   
   mLoadedLayoutPath = String(jsonFile).replace(ofToDataPath("").c_str(), "").toStdString();
   
   TheTitleBar->ListLayouts();
   if (makeDefaultLayout)
      UpdateUserPrefsLayout();
}

void ModularSynth::SaveLayoutAsPopup()
{
   FileChooser chooser("Save current layout as...", File(ofToDataPath("layouts/newlayout.json")));
   if (chooser.browseForFileToSave(true))
      SaveLayout(chooser.getResult().getRelativePathFrom(File(ofToDataPath(""))).toStdString());
}

void ModularSynth::SaveStatePopup()
{
   FileChooser chooser("Save current state as...", File(ofToDataPath("savestate/savestate.dat")));
   if (chooser.browseForFileToSave(true))
      SaveState(chooser.getResult().getRelativePathFrom(File(ofToDataPath(""))).toStdString());
}

void ModularSynth::LoadStatePopup()
{
   mShowLoadStatePopup = true;
}

void ModularSynth::LoadStatePopupImp()
{
   FileChooser chooser("Load state", File(ofToDataPath("savestate")));
   if (chooser.browseForFileToOpen())
      LoadState(chooser.getResult().getRelativePathFrom(File(ofToDataPath(""))).toStdString());
}

namespace
{
   const int kSaveStateRev = 420;
   const int kModuleSeparatorLength = 13;
   const char kModuleSeparator[kModuleSeparatorLength+1] = "ryanchallinor";
}

void ModularSynth::SaveState(string file)
{
   mAudioThreadMutex.Lock("SaveState()");
   
   SaveLayout(ofToDataPath(file)+".json", false);
   
   FileStreamOut out(ofToDataPath(file).c_str());
   
   out << kSaveStateRev;
   
   int savedModules = 0;
   for (auto* module : mModules)
   {
      if (module != TheSaveDataPanel && module != TheTitleBar)
         ++savedModules;
   }
   
   out << savedModules;
   
   for (auto* module : mModules)
   {
      if (module != TheSaveDataPanel && module != TheTitleBar)
      {
         //ofLog() << "Saving " << module->Name();
         out << string(module->Name());
         module->SaveState(out);
         for (int i=0; i<kModuleSeparatorLength; ++i)
            out << kModuleSeparator[i];
      }
   }
   
   mAudioThreadMutex.Unlock();
}

void ModularSynth::LoadState(string file)
{
   mAudioThreadMutex.Lock("LoadState()");
   
   LoadLayout(ofToDataPath(file)+".json", false);
   
   FileStreamIn in(ofToDataPath(file).c_str());
   
   int header;
   in >> header;
   assert(header == kSaveStateRev);
   
   int savedModules;
   in >> savedModules;
   
   for (int i=0; i<savedModules; ++i)
   {
      string moduleName;
      in >> moduleName;
      //ofLog() << "Loading " << moduleName;
      IDrawableModule* module = FindModule(moduleName, false);
      assert(module);
      try
      {
         module->LoadState(in);
         
         for (int j=0; j<kModuleSeparatorLength; ++j)
         {
            char separatorChar;
            in >> separatorChar;
            if (separatorChar != kModuleSeparator[j])
            {
               ofLog() << "Error loading state for " << module->Name();
               //something went wrong, let's print some info to try to figure it out
               ofLog() << "Read char " + ofToString(separatorChar) + " but expected " + kModuleSeparator[j] + "!";
               ofLog() << "Save state file position is " + ofToString(in.GetFilePosition()) + ", EoF is " + (in.Eof() ? "true" : "false");
               string nextFewChars = "Next 10 characters are:";
               for (int c=0;c<10;++c)
               {
                  char ch;
                  in >> ch;
                  nextFewChars += ofToString(ch);
               }
               ofLog() << nextFewChars;
            }
            assert(separatorChar == kModuleSeparator[j]);
         }
      }
      catch (LoadStateException& e)
      {
         LogEvent("Error loading state for module \""+moduleName+"\"", kLogEventType_Error);
         
         //read through the rest of the module until we find the spacer, so we can continue loading the next module
         int separatorProgress = 0;
         while (!in.Eof())
         {
            char val;
            in >> val;
            if (val == kModuleSeparator[separatorProgress])
               ++separatorProgress;
            else
               separatorProgress = 0;
            if (separatorProgress == kModuleSeparatorLength)
               break;   //we did it!
         }
      }
   }
   
   for (auto module : mModules)
      module->PostLoadState();
   
   TheTransport->Reset();
   
   mAudioThreadMutex.Unlock();
}

IAudioReceiver* ModularSynth::FindAudioReceiver(string name, bool fail)
{
   IAudioReceiver* a = NULL;
   
   if (name == "")
      return NULL;
   
   try
   {
      a = dynamic_cast<IAudioReceiver*>(FindModule(name,fail));
      if (a == NULL)
         throw WrongModuleTypeException();
   }
   catch (UnknownModuleException& e)
   {
      LogEvent("Couldn't find referenced audio receiver \""+name+"\"", kLogEventType_Error);
   }
   catch (WrongModuleTypeException& e)
   {
      LogEvent("\""+name+"\" is not an audio receiver", kLogEventType_Error);
   }
   
   return a;
}

INoteReceiver* ModularSynth::FindNoteReceiver(string name, bool fail)
{
   INoteReceiver* n = NULL;
   
   if (name == "")
      return NULL;
   
   try
   {
      n = dynamic_cast<INoteReceiver*>(FindModule(name,fail));
      if (n == NULL)
         throw WrongModuleTypeException();
   }
   catch (UnknownModuleException& e)
   {
      LogEvent("Couldn't find referenced note receiver \""+name+"\"", kLogEventType_Error);
   }
   catch (WrongModuleTypeException& e)
   {
      LogEvent("\""+name+"\" is not a note receiver", kLogEventType_Error);
   }
      
   return n;
}

void ModularSynth::OnConsoleInput()
{
   vector<string> tokens = ofSplitString(mConsoleText," ",true,true);
   if (tokens.size() > 0)
   {
      if (tokens[0] == "")
      {
      }
      else if (tokens[0] == "clearerrors")
      {
         mErrors.clear();
      }
      else if (tokens[0] == "clearall")
      {
         mAudioThreadMutex.Lock("clearall");
         ScopedLock renderLock(mRenderLock);
         ResetLayout();
         mAudioThreadMutex.Unlock();
      }
      else if (tokens[0] == "load")
      {
         LoadLayout(ofToDataPath(tokens[1]));
      }
      else if (tokens[0] == "save")
      {
         if (tokens.size() > 1)
            SaveLayout(ofToDataPath(tokens[1]));
         else
            SaveLayout();
      }
      else if (tokens[0] == "write")
      {
         SaveOutput();
      }
      else if (tokens[0] == "reconnect")
      {
         ReconnectMidiDevices();
      }
      else if (tokens[0] == "profiler")
      {
         Profiler::ToggleProfiler();
      }
      else if (tokens[0] == "clear")
      {
         mErrors.clear();
         mEvents.clear();
      }
      else if (tokens[0] == "minimizeall")
      {
         for (auto iter = mModules.begin(); iter != mModules.end(); ++iter)
         {
            (*iter)->SetMinimized(true);
         }
      }
      else if (tokens[0] == "resettime")
      {
         gTime = 0;
      }
      else if (tokens[0] == "hightime")
      {
         gTime += 1000000;
      }
      else if (tokens[0] == "tempo")
      {
         if (tokens.size() >= 2)
         {
            float tempo = atof(tokens[1].c_str());
            if (tempo > 0)
               TheTransport->SetTempo(tempo);
         }
      }
      else if (tokens[0] == "home")
      {
         mZoomer.GoHome();
      }
      else if (tokens[0] == "saveas")
      {
         SaveLayoutAsPopup();
      }
      else if (tokens[0] == "dev")
      {
         gShowDevModules = true;
         TheTitleBar->SetModuleFactory(&mModuleFactory);
      }
      else if (tokens[0] == "dumpmem")
      {
         DumpUnfreedMemory();
      }
      else if (tokens[0] == "savestate")
      {
         if (tokens.size() >= 2)
            SaveState("savestate/"+tokens[1]);
      }
      else if (tokens[0] == "loadstate")
      {
         if (tokens.size() >= 2)
            LoadState("savestate/"+tokens[1]);
      }
      else if (tokens[0] == "s")
      {
         SaveState("savestate/savestate.dat");
      }
      else if (tokens[0] == "l")
      {
         LoadState("savestate/savestate.dat");
      }
      else
      {
         ofLog() << "Creating: " << mConsoleText;
         ofVec2f grabOffset(-40,20);
         IDrawableModule* module = SpawnModuleOnTheFly(mConsoleText, GetMouseX() + grabOffset.x, GetMouseY() + grabOffset.y);
         TheSynth->SetMoveModule(module, grabOffset.x, grabOffset.y);
      }
   }
}

void ModularSynth::ClearConsoleInput()
{
   mConsoleText[0] = 0;
   mConsoleEntry->UpdateDisplayString();
}

IDrawableModule* ModularSynth::SpawnModuleOnTheFly(string moduleName, float x, float y)
{
   vector<string> tokens = ofSplitString(moduleName," ");
   if (tokens.size() == 0)
      return nullptr;

   ofxJSONElement dummy;
   dummy["type"] = tokens[0];
   dummy["name"] = GetUniqueName(tokens[0], mModules);

   if (tokens[0] == "effectchain")
   {
      for (int i=1; i<tokens.size(); ++i)
      {
         if (VectorContains(tokens[i],GetEffectFactory()->GetSpawnableEffects()))
         {
            ofxJSONElement effect;
            effect["type"] = tokens[i];
            dummy["effects"].append(effect);
         }
      }
   }

   dummy["position"][0u] = x;
   dummy["position"][1u] = y;

   IDrawableModule* module = NULL;
   try
   {
      ScopedMutex mutex(&mAudioThreadMutex, "CreateModule");
      module = CreateModule(dummy);
      SetUpModule(dummy);
      module->Init();
   }
   catch (LoadingJSONException& e)
   {
      LogEvent("Error spawning \""+moduleName+"\" on the fly", kLogEventType_Normal);
   }
   catch (UnknownModuleException& e)
   {
      LogEvent("Error spawning \""+moduleName+"\" on the fly, couldn't find \""+e.mSearchName+"\"", kLogEventType_Normal);
   }

   return module;
}

void ModularSynth::SetMoveModule(IDrawableModule* module, float offsetX, float offsetY)
{
   mMoveModule = module;
   mMoveModuleOffsetX = offsetX;
   mMoveModuleOffsetY = offsetY;
}

void ModularSynth::AddMidiDevice(MidiDevice* device)
{
   if (!VectorContains(device, mMidiDevices))
      mMidiDevices.push_back(device);
}

void ModularSynth::ReconnectMidiDevices()
{
   for (int i=0; i<mMidiDevices.size(); ++i)
      mMidiDevices[i]->Reconnect();
}

void ModularSynth::SaveOutput()
{
   ScopedMutex mutex(&mAudioThreadMutex, "SaveOutput()");

   string filename = ofGetTimestampString("recordings/recording_%m-%d-%Y_%H-%M.wav");
   //string filenamePos = ofGetTimestampString("recordings/pos_%m-%d-%Y_%H-%M.wav");

   assert(mRecordingLength <= RECORDING_LENGTH);
   
   for (int i=0; i<mRecordingLength; ++i)
   {
      mSaveOutputBuffer[i*2] = mOutputBufferLeft.GetSample(mRecordingLength-i-1);
      mSaveOutputBuffer[i*2+1] = mOutputBufferRight.GetSample(mRecordingLength-i-1);
   }

   Sample::WriteDataToFile(filename.c_str(), mSaveOutputBuffer, mRecordingLength*2, 2);
   
   //mOutputBufferMeasurePos.ReadChunk(mSaveOutputBuffer, mRecordingLength);
   //Sample::WriteDataToFile(filenamePos.c_str(), mSaveOutputBuffer, mRecordingLength, 1);
   
   mOutputBufferLeft.ClearBuffer();
   mOutputBufferRight.ClearBuffer();
   mOutputBufferMeasurePos.ClearBuffer();
   mRecordingLength = 0;
}

void ConsoleListener::TextEntryActivated(TextEntry* entry)
{
   TheSynth->ClearConsoleInput();
}

void ConsoleListener::TextEntryComplete(TextEntry* entry)
{
   TheSynth->OnConsoleInput();
}
