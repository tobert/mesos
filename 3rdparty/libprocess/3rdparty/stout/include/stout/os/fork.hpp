#ifndef __STOUT_OS_FORK_HPP__
#define __STOUT_OS_FORK_HPP__

#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <list>
#include <set>
#include <string>

#include <tr1/memory>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/process.hpp>

// Abstractions around forking process trees. You can declare a
// process tree "template" using 'Fork', 'Exec', and 'Wait'. For
// example, to describe a simple "fork/exec" you can do:
//
//   Fork f = Fork(Exec("sleep 10));
//
// The command passed to an 'Exec' is run via 'sh -c'. You can
// construct more complicated templates via nesting, for example:
//
//   Fork f =
//     Fork(None(),
//          Fork(Exec("echo 'grandchild 1'")),
//          Fork(None(),
//               Fork(Exec("echo 'great-grandchild'")),
//               Exec("echo 'grandchild 2'"))
//          Exec("echo 'child'"));
//
// Note that the first argument to 'Fork' here is an optional function
// that can be invoked before forking any more children or executing a
// command. THIS FUNCTION SHOULD BE ASYNC SIGNAL SAFE.
//
// To wait for children, you can use 'Wait' instead of 'Exec', for
// example:
//
//   Fork f =
//     Fork(None(),
//          Fork(Exec("echo 'grandchild 1'")),
//          Fork(Exec("echo 'grandchild 2'")),
//          Wait());
//
// You can also omit either an 'Exec' or a 'Wait' and the forked
// process will just 'exit(0)'. For example, the following will cause
// to processes to get reparented by 'init'.
//
//   Fork f =
//     Fork(None(),
//          Fork(Exec("echo 'grandchild 1'")),
//          Fork(Exec("echo 'grandchild 2'")));
//
// A template can be instantiated by invoking the 'Fork' as a
// functor. For example, using any of the templates above we can do:
//
//   Try<ProcessTree> tree = f();
//
// It's important to note that the process tree returned represents
// the instant in time after the forking has completed but before
// 'Exec', 'Wait' or 'exit(0)' has occured (i.e., the process tree
// will be complete).

namespace os {

// Forward declaration.
inline Result<Process> process(pid_t);


struct Exec
{
  Exec(const std::string& _command)
    : command(_command) {}

  const std::string command;
};


struct Wait {};


struct Fork
{
  //  -+- parent
  Fork(const Option<void(*)(void)>& _function,
       const Exec& _exec)
    : function(_function),
      exec(_exec) {}

  Fork(const Exec& _exec) : exec(_exec) {}

  //  -+- parent
  //   \--- child
  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1)
    : function(_function)
  {
    children.push_back(fork1);
  }

  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Exec& _exec)
    : function(_function),
      exec(_exec)
  {
    children.push_back(fork1);
  }

  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Wait& _wait)
    : function(_function),
      wait(_wait)
  {
    children.push_back(fork1);
  }

  //  -+- parent
  //   |--- child
  //   \--- child
  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Fork& fork2)
    : function(_function)
  {
    children.push_back(fork1);
    children.push_back(fork2);
  }

  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Fork& fork2,
       const Exec& _exec)
    : function(_function),
      exec(_exec)
  {
    children.push_back(fork1);
    children.push_back(fork2);
  }

  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Fork& fork2,
       const Wait& _wait)
    : function(_function),
      wait(_wait)
  {
    children.push_back(fork1);
    children.push_back(fork2);
  }

  //  -+- parent
  //   |--- child
  //   |--- child
  //   \--- child
  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Fork& fork2,
       const Fork& fork3)
    : function(_function)
  {
    children.push_back(fork1);
    children.push_back(fork2);
    children.push_back(fork3);
  }

  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Fork& fork2,
       const Fork& fork3,
       const Exec& _exec)
    : function(_function),
      exec(_exec)
  {
    children.push_back(fork1);
    children.push_back(fork2);
    children.push_back(fork3);
  }

  Fork(const Option<void(*)(void)>& _function,
       const Fork& fork1,
       const Fork& fork2,
       const Fork& fork3,
       const Wait& _wait)
    : function(_function),
      wait(_wait)
  {
    children.push_back(fork1);
    children.push_back(fork2);
    children.push_back(fork3);
  }

private:
  // We use shared memory to "share" the pids of forked descendants.
  // The benefit of shared memory over pipes is that each forked
  // process can read its descendants' pids leading to a simpler
  // implementation (with pipes, only one reader can ever read the
  // value from the pipe, forcing much more complicated coordination).
  //
  // Shared memory works like a file (in memory) that gets deleted by
  // "unlinking" it, but it won't get completely deleted until all
  // open file descriptors referencing it have been closed. Each
  // forked process has the shared memory mapped into it as well as an
  // open file descriptor, both of which should get cleaned up
  // automagically when the process exits, but we use a special
  // "deleter" (in combination with shared_ptr) in order to clean this
  // stuff up when we are actually finished using the shared memory.
  struct SharedMemoryDeleter
  {
    SharedMemoryDeleter(int _fd) : fd(_fd) {}

    void operator () (pid_t* pid) const
    {
      if (munmap(pid, sizeof(pid_t)) == -1) {
        perror("Failed to unmap memory");
        abort();
      }
      if (::close(fd) == -1) {
        perror("Failed to close shared memory file descriptor");
        abort();
      }
    }

    const int fd;
  };

  // Represents the "tree" of descendants where each node has a
  // pointer (into shared memory) from which we can read the
  // descendants pid as well as a vector of children.
  struct Tree
  {
    std::tr1::shared_ptr<pid_t> pid;
    std::vector<Tree> children;
  };

  // Constructs a Tree (see above) from this fork template.
  Try<Tree> prepare() const
  {
    static int forks = 0;

    // Each "instance" of an instantiated Fork needs a unique name for
    // creating shared memory.
    int instance = __sync_fetch_and_add(&forks, 1);

    std::string name =
      "/stout-forks-" + stringify(getpid()) + stringify(instance);

    int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

    if (fd == -1) {
      return ErrnoError("Failed to open a shared memory object");
    }

    if (ftruncate(fd, sizeof(pid_t)) == -1) {
      return ErrnoError("Failed to set size of shared memory object");
    }

    void* memory = mmap(
        NULL, sizeof(pid_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (memory == MAP_FAILED) {
      return ErrnoError("Failed to map shared memory object");
    }

    if (shm_unlink(name.c_str()) == -1) {
      return ErrnoError("Failed to unlink shared memory object");
    }

    SharedMemoryDeleter deleter(fd);

    Tree tree;
    tree.pid = std::tr1::shared_ptr<pid_t>((pid_t*) memory, deleter);
    *tree.pid = -1;

    for (size_t i = 0; i < children.size(); i++) {
      Try<Tree> tree_ = children[i].prepare();
      if (tree_.isError()) {
        return Error(tree_.error());
      }
      tree.children.push_back(tree_.get());
    }

    return tree;
  }

  // Performs the fork, executes the function, recursively
  // instantiates any children, and then executes/waits/exits.
  pid_t instantiate(const Tree& tree) const
  {
    pid_t pid = ::fork();
    if (pid > 0) {
      *tree.pid = pid;
      return pid;
    }

    // Child process.

    // Execute the function, if any.
    if (function.isSome()) {
      function.get()();
    }

    // Fork the children, if any.
    CHECK(children.size() == tree.children.size());
    std::set<pid_t> pids;
    for (size_t i = 0; i < children.size(); i++) {
      pids.insert(children[i].instantiate(tree.children[i]));
    }

    // Execute or wait.
    if (exec.isSome()) {
      // Execute the command (via '/bin/sh -c command').
      const char* command = exec.get().command.c_str();
      execl("/bin/sh", "sh", "-c", command, (char*) NULL);
      EXIT(1) << "Failed to execute '" << command << "': " << strerror(errno);
    } else if (wait.isSome()) {
      foreach (pid_t pid, pids) {
        // TODO(benh): Check for signal interruption or other errors.
        waitpid(pid, NULL, 0);
      }
    }

    exit(0);
    return -1;
  }

  // Waits for all of the descendant processes in the tree to update
  // their pids and constructs a ProcessTree by calling os::process
  // for each pid.
  static Try<ProcessTree> coordinate(const Tree& tree)
  {
    // Wait for the forked process.
    // TODO(benh): Don't wait forever?
    while (*tree.pid == -1) {
      // Make sure we don't keep reading the value from a register.
      __sync_synchronize();
    }

    Result<Process> process = os::process(*tree.pid);

    if (process.isError()) {
      return Error(process.error());
    } else if (process.isNone()) {
      // TODO(benh): Use a duplicate of the current process with
      // 'tree.pid' instead, or consider copying the 'Process' struct
      // via shared memory instead of just the pid.
      return Error("Process already terminated");
    }

    std::list<ProcessTree> children;
    for (size_t i = 0; i < tree.children.size(); i++) {
      Try<ProcessTree> child = coordinate(tree.children[i]);
      if (child.isError()) {
        return Error(child.error());
      }
      children.push_back(child.get());
    }

    return ProcessTree(process.get(), children);
  }

public:
  // Prepares and instantiates the process tree.
  Try<ProcessTree> operator () () const
  {
    Try<Tree> tree = prepare();

    if (tree.isError()) {
      return Error(tree.error());
    }

    Try<pid_t> pid = instantiate(tree.get());

    if (pid.isError()) {
      return Error(pid.error());
    }

    return coordinate(tree.get());
  }

private:
  Option<void(*)(void)> function;
  Option<const Exec> exec;
  Option<const Wait> wait;
  std::vector<Fork> children;
};

} // namespace os {

#endif // __STOUT_OS_FORK_HPP__
