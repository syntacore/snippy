# LLVM-snippy

LLVM-snippy is a cross-platform random code generator. Generation can be model-based or operate in an unmanaged mode. LLVM-snippy supports loop generation, function calls, memory patterns, and much more.

> [!IMPORTANT]
> Full documentation is available [here](https://syntacore.github.io/snippy/)

## Building and Installing Generator

In this section, we will cover the steps for building and installing LLVM-snippy.

### Dependencies
To build snippy install:

+ cmake
+ clang
+ ninja
+ python >= 3.8
+ ccache (optional but recommended) - to avoid this pass `-DLLVM_CCACHE_BUILD=OFF`
+ sphinx (optional) - to avoid this pass `-DLLVM_ENABLE_SPHINX=OFF` to cmake. Then no documentation will be built

### Build with cmake
1. Download the source code. For the presentation purposes, we assume that you download it to the `${SNIPPY_PATH}` directory.
1. From the `${SNIPPY_PATH}` directory, run CMake:

```
cmake -S llvm/ -B build/ --preset=snippy_basic -DCMAKE_BUILD_TYPE=Release
cmake --build build/ --target llvm-snippy llvm-ie
```

## Testing

You can test LLVM-snippy by running LIT tests located in `llvm/test/tools/llvm-snippy`. These tests are based on a common LLVM infrastructure that includes llvm-lit and FileCheck. For the additional information about the LLVM testing infrastructure, refer to [LLVM Testing Infrastructure Guide](https://llvm.org/docs/TestingGuide.html).

### Dependencies

To run tests, verify you have:

 * LLVM-snippy built
 * POSIX mandatory utilities (for example, grep, cat, etc.)

### Running LLVM-snippy Tests
Use the following command:

```
> cmake --build build/ --target check-llvm-tools-llvm-snippy
```

Alternatively, you can run llvm-lit directly:

```
> python3 build/bin/llvm-lit llvm/test/tools/llvm-snippy/
```

We expect that all tests pass with the `passed`, `unsupported`, or `xfail` statuses. If some of the tests fail for you, let us know by [creating an issue](#contributing-to-llvm-snippy).

Once done, LLVM-snippy should be up and running on your system. If you encounter any issues or have questions, feel free to reach out.

# Contributing to LLVM-snippy

Thank you for showing interest in contributing to LLVM-snippy. There are several ways you can help to make the generator better.

## Bug Reports

We want to know about all LLVM-snippy bugs: segmentation faults, poor or incorrect diagnostics, lack of randomization in the generated test, incorrectly working feature, etc. If you think that you have encountered a bug, file an issue on GitHub. Make sure that you provide the minimum description, reproduction and HEAD commit hash in the issue.

## Bug Fixes

We appreciate your desire to improve LLVM-snippy. You can start working on any open unassigned issue. A comment in the issue is enough to show that you have started working on it. If you do not have bandwidth to work on the issue already assigned to you, let other contributes know by leaving a comment in the issue, so it can be reassigned.

If the fix requires design discussion, create an [RFC](#driving-a-major-feature).

Regardless of the fix type, we encourage you to provide a short summary in the issue comments before creating a PR.

## Driving Major Features

If you want to introduce a major change or implement a new feature in LLVM-snippy, create an RFC first. We would like to keep LLVM-snippy community informed about major changes. We should also reach consensus on all technical and design decisions before any significant work is done.

There is no template for an RFC, so feel free to create it on your own. A good RFC contains:

- Overview
- Proposal
- Pros and cons
- Implementation steps

## Backward Compatibility

We try to keep backward compatibility in LLVM-snippy for our customers. Backward incompatible changes include, but are not limited to:
- Removing an existing option
- Changing an input configuration format
- Amending the signatures of entry functions or globals

These changes require a major release version change and must be discussed in prior. Please, create an [RFC](#driving-a-major-feature) for such changes.

## Submitting Patches

When your patch is ready for review, create a pull request. We ask you to follow these rules:
1. Adhere to the [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html) where applicable.
2. Run `git-clang-format` on your changes.
3. Remove any unrelated changes from your patch.
4. Add at least one lit-test at `llvm/test/tools/llvm-snippy` if possible.
4. Add unit tests at `llvm/unittests/tools/llvm-snippy` if you can
5. Make a single commit.
6. Create a PR from your fork, or use a branch that starts from `users/<username>/`.

You can request a review by mentioning people in the PR comments. Best candidates for review are developers who have contributed in the area your patch concerns. Keep in mind that the usual review period is one week, so do not ping more often.

Once your patch is reviewed and approved, you can merge the change. If you do not have the rights, let the maintainers know, so they could merge the change on your behalf.

Feel free to participate in any review you are interested in.

# Contacts

Use [GitHub issues and pull-requests](#contributing-to-llvm-snippy) to interact with the LLVM-snippy community, make suggestions and ask questions.
