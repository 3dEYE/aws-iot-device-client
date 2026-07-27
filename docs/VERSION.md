# Version

[*Back To The Main Readme*](../README.md)

## Application Version

The device client application version conforms to the [semantic version](https://semver.org/) naming convention and consists of the following components: `MAJOR.MINOR.PATCH-COMMIT`.

To retrieve the current installed version of Device Client, use the --version argument.

 ```
./aws-iot-device-client --version
```

The components of the version are derived at build time from the [CMakeLists.versioning](../CMakeLists.txt.versioning) script in the repo. This script is invoked from the toplevel CMakeLists.txt file used to build the software. The logic for obtaining each component of the version is described below:
* `MAJOR.MINOR` is derived from the nearest reachable numeric version tag whose complete name is `vMAJOR.MINOR` or `vMAJOR.MINOR.PATCH`.
    * Generated release tags such as `v1.9.12-abcdef0` are ignored so they cannot reset the next patch number.
* `PATCH` starts at zero when a reachable `vMAJOR.MINOR` line base exists, or at the patch from the nearest reachable `vMAJOR.MINOR.PATCH` tag when no bare base exists, then adds the commits since that base.
    * The commits-ahead portion is equivalent to `git rev-list <line-base-tag>..HEAD --count`.
* `COMMIT` is derived by obtaining the short sha of the current commit.
    * Equivalent to the output from `git rev-parse --short HEAD`.

### Process
* Changes to the `MAJOR` and/or `MINOR` version require the following steps.
    * Tag your commit in the repostory with `vMAJOR.MINOR`.
    * Update the `.version` file with updated `vMAJOR.MINOR` version.
        * Although `PATCH` and `COMMIT` are derived at build time, we follow a convention to set `PATCH` to 0 and `COMMIT` to the short sha associated with the tag.
* Changes to the `PATCH` or `COMMIT` require no action.
    * Do not update the `.version` file as these components of the version are determined at build time.

### FAQ

#### Under what circumstances will the `MAJOR` version increment?
The application `MAJOR` version will increment when any of the following are true:
* The device client introduces a backwards incompatible change to the public interface of any released software feature.
    * In this context, interface refers to a device client software interface and does not necessarily extend to other interfaces of the AWS IoT Device SDK.
    * Example of a backwards incompatible interface change.
        * Adding a new parameter without a default value to a public function.
    * Example of an interface change that is not considered backwards incompatible.
        * Adding a new public function without removing or changing existing public functions.
    * Users are required to update their software in order to compile and link with the new version.
* The device client introduces a backwards incompatible change to the configuration file used to start the device client.
    * Example of a backwards incompatible change to configuration.
        * Removing a previously supported feature from the configuration.
        * Changing a configuration setting from optional to required without also introducing a default value.
    * Example of a configuration change that is not considered backwards incompatible.
        * Introducing configuration for a new optional feature of device client.
    * Users are required to update their existing configuration files to the new format in order to use the new vesion of device cilent.
* Some other unanticipated major change to the device client not captured by the previous bullets.

#### Under what circumstances will the `MINOR` version increment?
The application `MINOR` version will increment when all of the following are true:
* The device client introduces a new feature, but this feature does not introduce any backwards incompatible changes to the behavior of other features.
* The device client introduces new configuration, but this configuration does not introduce any backwards incompatible changes to the configuration of other features.
* Some other significant change to the device client or its' dependencies that deserves more significant consideration by our users.
* See the discussion under `MAJOR` version for examples of changes that are considered backwards compatible and backwards incompatible.

#### Under what circumstances will the `PATCH` version increment?
The application `PATCH` version will increment when all of the following are true:
* Update to any dependency of the device client such as the AWS IoT Device SDK.
* Bugfixes to existing features that are resolved without introducing any backwards incompatible changes to the public interface or configuration.
* Changes to other artifacts in the repo such as documentation, code samples, or tests.
* See the discussion under `MAJOR` version for examples of changes that are considered backwards compatible and backwards incompatible.

#### Under what circumstances will the `COMMIT` version change?
The application `COMMIT` will change every time a commit is merged to the main branch.

[*Back To The Top*](#version)
