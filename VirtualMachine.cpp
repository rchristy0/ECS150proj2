#include "VirtualMachine.h"
#include "Machine.h"
#include "vector"
#include "stdlib.h"

#include "unistd.h"
#include "errno.h"

#include "iostream" //delete this
using namespace std; //also this

extern "C"
{
  //Implemented in VirtualMachineUtils.c
  TVMMainEntry VMLoadModule(const char *module);
  void VMUnloadModule(void);
  TVMStatus VMFilePrint(int filedescriptor, const char *format, ...);
  
  struct MCB;
  
  typedef struct TCB
  {
    TVMThreadID t_id;
    TVMThreadPriority t_prio;
    TVMThreadState t_state;
    TVMMemorySize t_memsize;
    TVMThreadEntry t_entry;
    uint8_t *stk_ptr;
    void *param;
    SMachineContext t_context;
    TVMTick t_ticks;
    int t_fileData;
    vector<MCB*> heldMutex;
  } TCB; // struct
  
  typedef struct MCB
  {
    unsigned int mutexID;
    unsigned int ownerID;
    vector<TCB*> waitHigh;
    vector<TCB*> waitNorm;
    vector<TCB*> waitLow;
  } MCB; //struct
  
  int curID;
  vector<TCB*> allThreads;
  vector<TCB*> readyHigh;
  vector<TCB*> readyNorm;
  vector<TCB*> readyLow;
  vector<TCB*> sleeping;
  vector<MCB*> allMutex;
  
  void setReady(TCB* thread)
  {
    TVMThreadPriority prio = thread->t_prio;
    switch (prio)
    {
      case VM_THREAD_PRIORITY_HIGH:
        readyHigh.push_back(thread);
        break;
      case VM_THREAD_PRIORITY_NORMAL:
        readyNorm.push_back(thread);
        break;
      case VM_THREAD_PRIORITY_LOW:
        readyLow.push_back(thread);
        break;
    }
  }
  
  void unReady(TCB* myThread)
  {
    switch(myThread->t_prio)
      {
        case VM_THREAD_PRIORITY_HIGH:
          for(unsigned int i = 0; i < readyHigh.size(); i++)
          {
            if (readyHigh[i]->t_id == myThread->t_id)
            {
              readyHigh.erase(readyHigh.begin() + i);
            }
          }
          break;
        case VM_THREAD_PRIORITY_NORMAL:
          for(unsigned int i = 0; i < readyNorm.size(); i++)
          {
            if (readyNorm[i]->t_id == myThread->t_id)
            {
              readyNorm.erase(readyNorm.begin() + i);
            }
          }
          break;
        case VM_THREAD_PRIORITY_LOW:
          for(unsigned int i = 0; i < readyLow.size(); i++)
          {
            if (readyLow[i]->t_id == myThread->t_id)
            {
              readyLow.erase(readyLow.begin() + i);
            }
          }
          break;
      }
  }
  
  void scheduler()
  {
    TVMThreadID tid;
    if(!readyHigh.empty())
    {
      tid = readyHigh[0]->t_id;
      readyHigh.erase(readyHigh.begin());
    }
    else if (!readyNorm.empty())
    {
      tid = readyNorm[0]->t_id;
      readyNorm.erase(readyNorm.begin());
    }
    else if (!readyLow.empty())
    {
      tid = readyLow[0]->t_id;
      readyLow.erase(readyLow.begin());
    }
    else
    {
      tid = 1; 
    }
    TCB *oldThread, *newThread;
    oldThread = allThreads[curID];
    newThread = allThreads[(int)tid];
    newThread->t_state = VM_THREAD_STATE_RUNNING;
    curID = tid;
    MachineContextSwitch(&oldThread->t_context, &newThread->t_context);
  }
  
  void AlarmCallback (void *calldata)
  {
    for(unsigned int i = 0; i < sleeping.size(); i++)
    {
      sleeping[i]->t_ticks--;
      if(sleeping[i]->t_ticks == 0)
      {
        sleeping[i]->t_state = VM_THREAD_STATE_READY;
        setReady(sleeping[i]);
        sleeping.erase(sleeping.begin()+i);
        scheduler();
      }  
    }
    TCB *curThread = allThreads[curID];
    curThread->t_state = VM_THREAD_STATE_READY;
    setReady(curThread);
    scheduler();
  }
  
  void FileIOCallback(void *calldata, int result)
  {
    TCB* myThread = (TCB*) calldata;
    myThread->t_fileData = result;
    myThread->t_state = VM_THREAD_STATE_READY;
    setReady(myThread);
    scheduler();
  }
  
  void threadWait(TCB* myThread)
  {
    myThread->t_state = VM_THREAD_STATE_WAITING;
    unReady(myThread);
    scheduler();
  }
  
  void skeleton(void * param)
  {
    TCB* thread = (TCB*) param;
    MachineEnableSignals();
    thread->t_entry(thread->param);
    VMThreadTerminate(thread->t_id);
  }
  
  void idleFunc(void *)
  {
    MachineEnableSignals();
    while (1);
  }
 
  TVMStatus VMStart(int tickms, int machinetickms, int argc, char *argv[])
  {
    curID = 0;
    const char *module = argv[0];
    TCB *mainThread = (TCB*)malloc(sizeof(TCB));
    mainThread->t_id = 0;
    mainThread->t_prio = VM_THREAD_PRIORITY_NORMAL;
    mainThread->t_state = VM_THREAD_STATE_READY;
    allThreads.push_back(mainThread);
    
    TVMThreadID idleID;
    VMThreadCreate(idleFunc, NULL, 0x100000, 0, &idleID);
    VMThreadActivate(idleID);
    
    TVMMainEntry VMMain = VMLoadModule(module);
    if (VMMain == NULL)
    {
      return VM_STATUS_FAILURE;
    }

    MachineInitialize(machinetickms);
    MachineRequestAlarm(tickms * 1000, AlarmCallback, NULL);
    MachineEnableSignals();
    VMMain(argc, argv);
    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(entry == NULL || tid == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    TCB *newThread = (TCB *)malloc(sizeof(TCB));
    newThread->t_state = VM_THREAD_STATE_DEAD;
    newThread->t_entry = entry;
    newThread->param = param;
    newThread->t_memsize = memsize;
    newThread->stk_ptr = new uint8_t[memsize];
    newThread->t_prio = prio;
    *tid = allThreads.size();
    newThread->t_id = *tid;
    allThreads.push_back(newThread);
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadDelete(TVMThreadID thread)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(thread < 0 || thread > allThreads.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    TCB *myThread = allThreads[(int)thread];
    if(myThread->t_state != VM_THREAD_STATE_DEAD)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_STATE;
    }
    allThreads.erase(allThreads.begin() + myThread->t_id);
    allThreads.insert(allThreads.begin() + myThread->t_id, NULL); //fill removed spot with null so tid still corresponds to index in allThreads
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadActivate(TVMThreadID thread)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    TCB *myThread = allThreads[(int)thread];
    MachineContextCreate(&myThread->t_context, skeleton, myThread, myThread->stk_ptr, myThread->t_memsize);
    myThread->t_state = VM_THREAD_STATE_READY;
    setReady(myThread);
    if(myThread->t_prio > allThreads[curID]->t_prio)
    {
      scheduler();
    }
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadTerminate(TVMThreadID thread)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(thread < 0 || thread > allThreads.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    TCB *myThread = allThreads[(int)thread];
    if(myThread->t_state == VM_THREAD_STATE_DEAD)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_STATE;
    }
    if(myThread->t_state == VM_THREAD_STATE_WAITING)
    {
      for(unsigned int i = 0; i < sleeping.size(); i++)
      {
        if (sleeping[i]->t_id == myThread->t_id)
        {
          sleeping.erase(sleeping.begin() + i);
        }
      }
    }
    else
    {
      unReady(myThread);
    }
    myThread->t_state = VM_THREAD_STATE_DEAD;
    for(unsigned int i = 0; i < myThread->heldMutex.size(); i++)
    {
      VMMutexRelease(myThread->heldMutex[i]->mutexID);
    }
    scheduler();
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadID(TVMThreadIDRef threadref)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(threadref == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *threadref = curID;
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(thread < 0 || thread > allThreads.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    if(stateref == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *stateref = allThreads[thread]->t_state;
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadSleep(TVMTick tick)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(tick == VM_TIMEOUT_INFINITE)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    } 
    else if (tick == VM_TIMEOUT_IMMEDIATE)
    {
      allThreads[curID]->t_state = VM_THREAD_STATE_READY;
      setReady(allThreads[curID]);
      scheduler();
    }
    else
    {
    allThreads[curID]->t_ticks = tick;
    allThreads[curID]->t_state = VM_THREAD_STATE_WAITING;
    sleeping.push_back(allThreads[curID]);
    scheduler();
    }
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(mutexref == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    MCB *newMutex = (MCB*)malloc(sizeof(MCB));
    *mutexref = allMutex.size();
    newMutex->mutexID = *mutexref;
    newMutex->ownerID = VM_THREAD_ID_INVALID;
    allMutex.push_back(newMutex);
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMutexDelete(TVMMutexID mutex)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(mutex < 0 || mutex > allMutex.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    MCB *myMutex = allMutex[(int)mutex];
    if(myMutex->ownerID != VM_THREAD_ID_INVALID)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_STATE;
    }
    allMutex.erase(allMutex.begin() + myMutex->mutexID);
    allMutex.insert(allMutex.begin() + myMutex->mutexID, NULL);
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
  { 
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(ownerref == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    if(mutex < 0 || mutex > allMutex.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    MCB *myMutex = allMutex[mutex];
    *ownerref = myMutex->ownerID;
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(mutex < 0 || mutex > allMutex.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    MCB *myMutex = allMutex[(int)mutex];
    TCB *myThread = allThreads[curID];
    cout << myThread->t_id << " acquiring ";
    cout << myMutex->mutexID << " with ";
    cout << timeout <<'\n';
    if(myMutex->ownerID == VM_THREAD_ID_INVALID)
    {
      myMutex->ownerID = myThread->t_id;
      myThread->heldMutex.push_back(myMutex);
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else if (timeout == VM_TIMEOUT_IMMEDIATE)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
    }
    else if (timeout == VM_TIMEOUT_INFINITE)
    {
      myThread->t_state = VM_THREAD_STATE_WAITING;
      TVMThreadPriority prio = myThread->t_prio;
      switch (prio)
      {
        case VM_THREAD_PRIORITY_HIGH:
          myMutex->waitHigh.push_back(myThread);
          break;
        case VM_THREAD_PRIORITY_NORMAL:
          myMutex->waitNorm.push_back(myThread);
          break;
        case VM_THREAD_PRIORITY_LOW:
          myMutex->waitLow.push_back(myThread);
          break;
      }
      scheduler();
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else
    {
      cout<<"going to sleep\n";
      VMThreadSleep(timeout);
      return VMMutexAcquire(mutex, VM_TIMEOUT_IMMEDIATE);
    } 
  }  
  
  TVMStatus VMMutexRelease(TVMMutexID mutex)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(mutex < 0 || mutex > allMutex.size())
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    MCB *myMutex = allMutex[(int)mutex];
    TCB *myThread = allThreads[curID];
    if(myMutex->ownerID != myThread->t_id)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_STATE;
    }
    myMutex->ownerID = VM_THREAD_ID_INVALID;
    TCB *tempThread;
    if(!myMutex->waitHigh.empty())
    {
      tempThread = myMutex->waitHigh[0];
      myMutex->waitHigh.erase(myMutex->waitHigh.begin());
      setReady(tempThread);
    }
    else if (!myMutex->waitNorm.empty())
    {
      tempThread = myMutex->waitNorm[0];
      myMutex->waitNorm.erase(myMutex->waitNorm.begin());
      setReady(tempThread);
    }
    else if (!myMutex->waitLow.empty())
    {
      tempThread = myMutex->waitLow[0];
      myMutex->waitLow.erase(myMutex->waitLow.begin());
      setReady(tempThread);
    }
    else
    {
      tempThread = NULL;
    }
    if(tempThread != NULL && tempThread->t_prio > myThread->t_prio)
    {
      scheduler();
    }
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if(filename == NULL || filedescriptor == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    TCB *myThread = allThreads[curID];
    MachineFileOpen(filename, flags, mode, FileIOCallback, myThread);
    threadWait(myThread);
    *filedescriptor = myThread->t_fileData;
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMFileClose(int filedescriptor)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    TCB *myThread = allThreads[curID];
    MachineFileClose(filedescriptor, FileIOCallback, myThread);
    threadWait(myThread);
    if(myThread->t_fileData < 0)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
    }  
  } 
  
  TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    TCB *myThread = allThreads[curID];
    MachineFileRead(filedescriptor, data, *length, FileIOCallback, myThread);
    threadWait(myThread);
    *length = myThread->t_fileData;
    if (length >= 0)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
    }
  }
  
  TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    if (data == NULL || length == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    TCB *myThread = allThreads[curID];
    MachineFileWrite(filedescriptor, data, *length, FileIOCallback, myThread);
    threadWait(myThread);
    *length = myThread->t_fileData;
    if (length >= 0)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
    }
  }
  
  TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    TCB *myThread = allThreads[curID];
    MachineFileSeek(filedescriptor,offset, whence, FileIOCallback, myThread);
    threadWait(myThread);
    if (newoffset != NULL)
    {
      *newoffset = myThread->t_fileData;
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
    }
  }
}