// Filename: p3dSession.cxx
// Created by:  drose (03Jun09)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) Carnegie Mellon University.  All rights reserved.
//
// All use of this software is subject to the terms of the revised BSD
// license.  You should have received a copy of this license along
// with this source code in a file named "LICENSE."
//
////////////////////////////////////////////////////////////////////

#include "p3dSession.h"
#include "p3dInstance.h"
#include "p3dInstanceManager.h"

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::Constructor
//       Access: Public
//  Description: Creates a new session, corresponding to a new
//               subprocess with its own copy of Python.  The initial
//               parameters for the session are taken from the
//               indicated instance object (but the instance itself is
//               not automatically started within the session).
////////////////////////////////////////////////////////////////////
P3DSession::
P3DSession(P3DInstance *inst) {
  _session_key = inst->get_session_key();
  _python_version = inst->get_python_version();
  _python_root_dir = "C:/p3drun";

  _python_state = PS_init;
  _started_read_thread = false;
  _read_thread_continue = false;

  _output_filename = inst->lookup_token("output_filename");

  INIT_LOCK(_instances_lock);
}


////////////////////////////////////////////////////////////////////
//     Function: P3DSession::Destructor
//       Access: Public
//  Description: Terminates the session by shutting down Python and
//               stopping the subprocess.
////////////////////////////////////////////////////////////////////
P3DSession::
~P3DSession() {
  if (_python_state == PS_running) {
    // Tell the process we're going away.
    TiXmlDocument doc;
    TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "", "");
    TiXmlElement *xcommand = new TiXmlElement("command");
    xcommand->SetAttribute("cmd", "exit");
    doc.LinkEndChild(decl);
    doc.LinkEndChild(xcommand);
    _pipe_write << doc << flush;

    // Also close the pipe, to help underscore the point.
    _pipe_write.close();
    _pipe_read.close();

#ifdef _WIN32
    // Now give the process a chance to terminate itself cleanly.
    if (WaitForSingleObject(_p3dpython.hProcess, 2000) == WAIT_TIMEOUT) {
      // It didn't shut down cleanly, so kill it the hard way.
      cerr << "Terminating process.\n";
      TerminateProcess(_p3dpython.hProcess, 2);
    }

    CloseHandle(_p3dpython.hProcess);
    CloseHandle(_p3dpython.hThread);
#endif
  }

  // If there are any leftover commands in the queue (presumably
  // implying we have never started the python process), then delete
  // them now, unsent.
  Commands::iterator ci;
  for (ci = _commands.begin(); ci != _commands.end(); ++ci) {
    delete (*ci);
  }
  _commands.clear();

  join_read_thread();
  DESTROY_LOCK(_instances_lock);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::start_instance
//       Access: Public
//  Description: Adds the indicated instance to the session, and
//               starts it running.  It is an error if the instance
//               has been started anywhere else.
//
//               The instance must have the same session_key as the
//               one that was passed to the P3DSession constructor.
////////////////////////////////////////////////////////////////////
void P3DSession::
start_instance(P3DInstance *inst) {
  assert(inst->_session == NULL);
  assert(inst->get_session_key() == _session_key);
  assert(inst->get_python_version() == _python_version);

  ACQUIRE_LOCK(_instances_lock);
  inst->_session = this;
  bool inserted = _instances.insert(Instances::value_type(inst->get_instance_id(), inst)).second;
  RELEASE_LOCK(_instances_lock);
  assert(inserted);

  TiXmlDocument *doc = new TiXmlDocument;
  TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "", "");
  TiXmlElement *xcommand = new TiXmlElement("command");
  xcommand->SetAttribute("cmd", "start_instance");
  TiXmlElement *xinstance = inst->make_xml();
  
  doc->LinkEndChild(decl);
  doc->LinkEndChild(xcommand);
  xcommand->LinkEndChild(xinstance);

  send_command(doc);

  if (_python_state == PS_init) {
    download_p3dpython(inst);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::terminate_instance
//       Access: Public
//  Description: Removes the indicated instance from the session, and
//               stops it.  It is an error if the instance is not
//               already running on this session.
////////////////////////////////////////////////////////////////////
void P3DSession::
terminate_instance(P3DInstance *inst) {
  TiXmlDocument *doc = new TiXmlDocument;
  TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "", "");
  TiXmlElement *xcommand = new TiXmlElement("command");
  xcommand->SetAttribute("cmd", "terminate_instance");
  xcommand->SetAttribute("id", inst->get_instance_id());
  
  doc->LinkEndChild(decl);
  doc->LinkEndChild(xcommand);

  send_command(doc);

  ACQUIRE_LOCK(_instances_lock);
  if (inst->_session == this) {
    inst->_session = NULL;
    _instances.erase(inst->get_instance_id());
  }
  RELEASE_LOCK(_instances_lock);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::send_command
//       Access: Private
//  Description: Sends the indicated command to the running Python
//               process.  If the process has not yet been started,
//               queues it up until it is ready.
//
//               The command must be a newly-allocated TiXmlDocument;
//               it will be deleted after it has been delivered to the
//               process.
////////////////////////////////////////////////////////////////////
void P3DSession::
send_command(TiXmlDocument *command) {
  if (_python_state == PS_running) {
    // Python is running.  Send the command.
    _pipe_write << *command << flush;
    delete command;
  } else {
    // Python not yet running.  Queue up the command instead.
    _commands.push_back(command);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::download_p3dpython
//       Access: Private
//  Description: Starts the Python package downloading.  Once it is
//               fully downloaded and unpacked, automatically calls
//               start_p3dpython.
////////////////////////////////////////////////////////////////////
void P3DSession::
download_p3dpython(P3DInstance *inst) {
  P3DFileDownload *download = new P3DFileDownload();
  download->set_url("http://fewmet/~drose/p3drun.tgz");

  string local_filename = "temp.tgz";
  if (!download->set_filename(local_filename)) {
    cerr << "Could not open " << local_filename << "\n";
    return;
  }

  inst->start_download(download);

  //  start_p3dpython();
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::start_p3dpython
//       Access: Private
//  Description: Starts Python running in a child process.
////////////////////////////////////////////////////////////////////
void P3DSession::
start_p3dpython() {
  string p3dpython = "c:/cygwin/home/drose/player/direct/built/bin/p3dpython.exe";
  //  string p3dpython = _python_root_dir + "/p3dpython.exe";

  // Populate the new process' environment.
  string env;

  // These are the enviroment variables we forward from the current
  // environment, if they are set.
  const char *keep[] = {
    "TEMP", "HOME", "USER", 
#ifdef _WIN32
    "SYSTEMROOT",
#endif
    NULL
  };
  for (int ki = 0; keep[ki] != NULL; ++ki) {
    char *value = getenv(keep[ki]);
    if (value != NULL) {
      env += keep[ki];
      env += "=";
      env += value;
      env += '\0';
    }
  }

  // Define some new environment variables.
  env += "PATH=";
  env += _python_root_dir;
  env += '\0';

  env += "PYTHONPATH=";
  env += _python_root_dir;
  env += '\0';

  // Create a bi-directional pipe to communicate with the sub-process.
#ifdef _WIN32
  HANDLE r_to, w_to, r_from, w_from;

  // Create the pipe to the process.
  if (!CreatePipe(&r_to, &w_to, NULL, 0)) {
    cerr << "failed to create pipe\n";
  } else {
    // Make sure the right end of the pipe is inheritable.
    SetHandleInformation(r_to, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(w_to, HANDLE_FLAG_INHERIT, 0);
  }

  // Create the pipe from the process.
  if (!CreatePipe(&r_from, &w_from, NULL, 0)) {
    cerr << "failed to create pipe\n";
  } else { 
    // Make sure the right end of the pipe is inheritable.
    SetHandleInformation(w_from, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(r_from, HANDLE_FLAG_INHERIT, 0);
  }

  HANDLE error_handle = GetStdHandle(STD_ERROR_HANDLE);
  bool got_output_filename = !_output_filename.empty();
  if (got_output_filename) {
    // Open the named file for output and redirect the child's stderr
    // into it.
    HANDLE handle = CreateFile
      (_output_filename.c_str(), GENERIC_WRITE, 
       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
       NULL, CREATE_ALWAYS, 0, NULL);
    if (handle != INVALID_HANDLE_VALUE) {
      error_handle = handle;
      SetHandleInformation(error_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    } else {
      cerr << "Unable to open " << _output_filename << "\n";
    }
  }

  // Make sure we see an error dialog if there is a missing DLL.
  SetErrorMode(0);

  // Pass the appropriate ends of the bi-directional pipe as the
  // standard input and standard output of the child process.
  STARTUPINFO startup_info;
  ZeroMemory(&startup_info, sizeof(STARTUPINFO));
  startup_info.cb = sizeof(startup_info); 
  startup_info.hStdError = error_handle;
  startup_info.hStdOutput = w_from;
  startup_info.hStdInput = r_to;
  startup_info.dwFlags |= STARTF_USESTDHANDLES;

  // Make sure the "python" console window is hidden.
  startup_info.wShowWindow = SW_HIDE;
  startup_info.dwFlags |= STARTF_USESHOWWINDOW;
 
  BOOL result = CreateProcess
    (p3dpython.c_str(), NULL, NULL, NULL, TRUE, 0,
     (void *)env.c_str(), _python_root_dir.c_str(),
     &startup_info, &_p3dpython);
  bool started_p3dpython = (result != 0);

  // Close the pipe handles that are now owned by the child.
  CloseHandle(w_from);
  CloseHandle(r_to);
  if (got_output_filename) {
    CloseHandle(error_handle);
  }

  _pipe_read.open_read(r_from);
  _pipe_write.open_write(w_to);
#endif  // _WIN32

  if (!started_p3dpython) {
    cerr << "Failed to create process.\n";
    return;
  }
  _python_state = PS_running;

  cerr << "Created process: " << _p3dpython.dwProcessId << "\n";

  if (!_pipe_read) {
    cerr << "unable to open read pipe\n";
  }
  if (!_pipe_write) {
    cerr << "unable to open write pipe\n";
  }
  
  spawn_read_thread();

  // Now that the process has been started, feed it any commands we
  // may have queued up.
  Commands::iterator ci;
  for (ci = _commands.begin(); ci != _commands.end(); ++ci) {
    _pipe_write << *(*ci);
    delete (*ci);
  }
  _pipe_write << flush;
  _commands.clear();
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::spawn_read_thread
//       Access: Private
//  Description: Starts the read thread.  This thread is responsible
//               for reading the standard input socket for XML
//               requests and storing them in the _requests queue.
////////////////////////////////////////////////////////////////////
void P3DSession::
spawn_read_thread() {
  assert(!_read_thread_continue);

  // We have to use direct OS calls to create the thread instead of
  // Panda constructs, because it has to be an actual thread, not
  // necessarily a Panda thread (we can't use Panda's simple threads
  // implementation, because we can't get overlapped I/O on an
  // anonymous pipe in Windows).

  _read_thread_continue = true;
#ifdef _WIN32
  _read_thread = CreateThread(NULL, 0, &win_rt_thread_run, this, 0, NULL);
#endif
  _started_read_thread = true;
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::join_read_thread
//       Access: Private
//  Description: Waits for the read thread to stop.
////////////////////////////////////////////////////////////////////
void P3DSession::
join_read_thread() {
  if (!_started_read_thread) {
    return;
  }

  cerr << "session waiting for thread\n";
  _read_thread_continue = false;
  _pipe_read.close();
  
#ifdef _WIN32
  assert(_read_thread != NULL);
  WaitForSingleObject(_read_thread, INFINITE);
  CloseHandle(_read_thread);
  _read_thread = NULL;
#endif
  cerr << "session done waiting for thread\n";

  _started_read_thread = false;
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::rt_thread_run
//       Access: Private
//  Description: The main function for the read thread.
////////////////////////////////////////////////////////////////////
void P3DSession::
rt_thread_run() {
  cerr << "session thread reading.\n";
  while (_read_thread_continue) {
    TiXmlDocument *doc = new TiXmlDocument;

    _pipe_read >> *doc;
    if (!_pipe_read || _pipe_read.eof()) {
      // Some error on reading.  Abort.
      cerr << "Error on session reading.\n";
      rt_terminate();
      return;
    }

    // Successfully read an XML document.
    cerr << "Session got request: " << *doc << "\n";

    // TODO: feed the request up to the parent.
    delete doc;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: P3DSession::rt_terminate
//       Access: Private
//  Description: Got a closed pipe from the sub-process.  Send a
//               terminate request for all instances.
////////////////////////////////////////////////////////////////////
void P3DSession::
rt_terminate() {
  Instances icopy;
  ACQUIRE_LOCK(_instances_lock);
  icopy = _instances;
  RELEASE_LOCK(_instances_lock);

  // TODO: got a race condition here.  What happens if someone deletes
  // an instance while we're processing this loop?

  for (Instances::iterator ii = icopy.begin(); ii != icopy.end(); ++ii) {
    P3DInstance *inst = (*ii).second;
    P3D_request *request = new P3D_request;
    request->_instance = inst;
    request->_request_type = P3D_RT_stop;
    inst->add_request(request);
  }
}

#ifdef _WIN32
////////////////////////////////////////////////////////////////////
//     Function: P3DPython::win_rt_thread_run
//       Access: Private, Static
//  Description: The Windows flavor of the thread callback function.
////////////////////////////////////////////////////////////////////
DWORD P3DSession::
win_rt_thread_run(LPVOID data) {
  ((P3DSession *)data)->rt_thread_run();
  return 0;
}
#endif
