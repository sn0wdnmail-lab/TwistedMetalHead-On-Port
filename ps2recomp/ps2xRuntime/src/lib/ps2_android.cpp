// Android version: the app's folders, the activity's state, storage permission and vibration
// (see runtime/ps2_android.h). The Java side is TmhoActivity (port/android), a NativeActivity
// with a few methods called from here; without it (a plain NativeActivity) the calls fall back.

#include "runtime/ps2_android.h"

#if defined(__ANDROID__)

#include "raylib.h"

#include <GLES3/gl3.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern "C" struct android_app *GetAndroidApp(void); // raylib (rcore_android.c)

// raylib's OpenGL ES 3 build calls the EXT_draw_buffers name, which is core in ES 3 (and not
// exported by libGLESv3 under that name).
extern "C" void glDrawBuffersEXT(GLsizei count, const GLenum *buffers)
{
    glDrawBuffers(count, buffers);
}

namespace ps2android
{
    namespace
    {
        void makeDirectories(const std::string &path)
        {
            std::string partial;
            for (size_t i = 0; i < path.size(); ++i)
            {
                partial.push_back(path[i]);
                if ((path[i] == '/' && i > 0) || i + 1 == path.size())
                    ::mkdir(partial.c_str(), 0775);
            }
        }

        // A thread attached to Java here is detached again when it ends (Android stops the app when
        // an attached thread ends without that).
        struct JavaAttachment
        {
            JavaVM *vm = nullptr;
            ~JavaAttachment()
            {
                if (vm)
                    vm->DetachCurrentThread();
            }
        };

        // The calling thread's JNIEnv (normally the app's main thread, which runs android_main).
        JNIEnv *javaEnv()
        {
            android_app *app = GetAndroidApp();
            if (!app || !app->activity || !app->activity->vm)
                return nullptr;
            JavaVM *vm = app->activity->vm;
            JNIEnv *env = nullptr;
            if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK)
                return env;
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
                return nullptr;
            thread_local JavaAttachment attachment;
            attachment.vm = vm;
            return env;
        }

        // A no-argument method of the activity; false when there is none (a plain NativeActivity).
        bool callActivity(const char *name, const char *signature, jvalue *args, jboolean *boolResult)
        {
            JNIEnv *env = javaEnv();
            android_app *app = GetAndroidApp();
            if (!env || !app || !app->activity || !app->activity->clazz)
                return false;
            jobject activity = app->activity->clazz;
            jclass type = env->GetObjectClass(activity);
            jmethodID method = env->GetMethodID(type, name, signature);
            if (!method)
            {
                env->ExceptionClear();
                env->DeleteLocalRef(type);
                return false;
            }
            if (boolResult)
                *boolResult = env->CallBooleanMethodA(activity, method, args);
            else
                env->CallVoidMethodA(activity, method, args);
            const bool threw = env->ExceptionCheck();
            if (threw)
                env->ExceptionClear();
            env->DeleteLocalRef(type);
            return !threw;
        }
    }

    bool active() noexcept { return true; }

    std::string dataDirectory()
    {
        static const std::string directory = []()
        {
            android_app *app = GetAndroidApp();
            std::string path;
            if (app && app->activity)
            {
                if (app->activity->externalDataPath && *app->activity->externalDataPath)
                    path = app->activity->externalDataPath;
                else if (app->activity->internalDataPath)
                    path = app->activity->internalDataPath;
            }
            if (path.empty())
                return std::string();
            makeDirectories(path);
            if (path.back() != '/')
                path.push_back('/');
            return path;
        }();
        return directory;
    }

    bool inBackground() noexcept
    {
        android_app *app = GetAndroidApp();
        if (!app || !app->window)
            return true;
        return app->activityState != APP_CMD_RESUME || !IsWindowFocused();
    }

    bool hasStorageAccess()
    {
        jboolean granted = JNI_FALSE;
        if (callActivity("hasStorageAccess", "()Z", nullptr, &granted))
            return granted == JNI_TRUE;
        // No Java side: try to list the shared storage.
        DIR *dir = ::opendir("/storage/emulated/0/Download");
        if (!dir)
            return false;
        ::closedir(dir);
        return true;
    }

    void requestStorageAccess()
    {
        callActivity("requestStorageAccess", "()V", nullptr, nullptr);
    }

    void vibrate(int milliseconds)
    {
        if (milliseconds <= 0)
            return;
        jvalue arg;
        arg.i = milliseconds;
        callActivity("vibrate", "(I)V", &arg, nullptr);
    }

    std::vector<std::string> storageRoots()
    {
        std::vector<std::string> roots{"/storage/emulated/0"};
        if (DIR *dir = ::opendir("/storage"))
        {
            while (dirent *entry = ::readdir(dir))
            {
                const std::string name = entry->d_name;
                if (name.empty() || name[0] == '.' || name == "emulated" || name == "self")
                    continue;
                roots.push_back("/storage/" + name); // SD cards: /storage/XXXX-XXXX
            }
            ::closedir(dir);
        }
        return roots;
    }

    void finishActivity()
    {
        if (callActivity("quitApp", "()V", nullptr, nullptr))
            return;
        if (android_app *app = GetAndroidApp(); app && app->activity)
            ANativeActivity_finish(app->activity);
    }

    std::string displayPath(const std::string &path)
    {
        static const char *const kShared = "/storage/emulated/0/";
        if (path.rfind(kShared, 0) == 0)
            return "your phone's storage, " + path.substr(std::strlen(kShared));
        return path;
    }
}

#else

namespace ps2android
{
    bool active() noexcept { return false; }
    std::string dataDirectory() { return std::string(); }
    bool inBackground() noexcept { return false; }
    bool hasStorageAccess() { return true; }
    void requestStorageAccess() {}
    void vibrate(int) {}
    std::vector<std::string> storageRoots() { return {}; }
    std::string displayPath(const std::string &path) { return path; }
    void finishActivity() {}
}

#endif
