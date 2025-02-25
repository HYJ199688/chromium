// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import groovy.json.JsonOutput
import org.gradle.api.DefaultTask
import org.gradle.api.tasks.TaskAction

import java.util.regex.Pattern

/**
 * Task to download dependencies specified in {@link ChromiumPlugin} and configure the
 * Chromium build to integrate them. Used by declaring a new task in a {@code build.gradle}
 * file:
 * <pre>
 * task myTaskName(type: BuildConfigGenerator) {
 *   repositoryPath 'build_files_and_repository_location/'
 * }
 * </pre>
 */
class BuildConfigGenerator extends DefaultTask {
    private static final BUILD_GN_TOKEN_START = "# === Generated Code Start ==="
    private static final BUILD_GN_TOKEN_END = "# === Generated Code End ==="
    private static final BUILD_GN_GEN_PATTERN = Pattern.compile(
            "${BUILD_GN_TOKEN_START}(.*)${BUILD_GN_TOKEN_END}",
            Pattern.DOTALL)
    private static final BUILD_GN_GEN_REMINDER = "# This is generated, do not edit. Update BuildConfigGenerator.groovy instead.\n"
    private static final DEPS_TOKEN_START = "# === ANDROID_DEPS Generated Code Start ==="
    private static final DEPS_TOKEN_END = "# === ANDROID_DEPS Generated Code End ==="
    private static final DEPS_GEN_PATTERN = Pattern.compile(
            "${DEPS_TOKEN_START}(.*)${DEPS_TOKEN_END}",
            Pattern.DOTALL)
    private static final DOWNLOAD_DIRECTORY_NAME = "libs"
    // This must be unique, so better be safe and increment the suffix rather than resetting
    // to cr0.
    private static final CIPD_SUFFIX = "cr0"

    // Some libraries are hosted in Chromium's //third_party directory. This is a mapping between
    // them so they can be used instead of android_deps pulling in its own copy.
    private static final def EXISTING_LIBS = [
        'junit_junit': '//third_party/junit:junit',
        'org_hamcrest_hamcrest_core': '//third_party/hamcrest:hamcrest_core_java',
    ]


    /**
     * Directory where the artifacts will be downloaded and where files will be generated.
     * Note: this path is specified as relative to the chromium source root, and must be normalised
     * to an absolute path before being used, as Groovy would base relative path where the script
     * is being executed.
     */
    String repositoryPath

    @TaskAction
    void main() {
        def graph = new ChromiumDepGraph(project: project)
        def normalisedRepoPath = normalisePath(repositoryPath)
        def rootDirPath = normalisePath(".")

        // 1. Parse the dependency data
        graph.collectDependencies()

        // 2. Import artifacts into the local repository
        def dependencyDirectories = []
        graph.dependencies.values().each { dependency ->
            if (dependency.exclude || EXISTING_LIBS.get(dependency.id) != null) {
                return
            }
            logger.debug "Processing ${dependency.name}: \n${jsonDump(dependency)}"
            def depDir = "${DOWNLOAD_DIRECTORY_NAME}/${dependency.id}"
            def absoluteDepDir = "${normalisedRepoPath}/${depDir}"

            dependencyDirectories.add(depDir)

            if (new File("${absoluteDepDir}/${dependency.fileName}").exists()) {
                getLogger().quiet("${dependency.id} exists, skipping.")
                return
            }

            project.copy {
                from dependency.artifact.file
                into absoluteDepDir
            }

            new File("${absoluteDepDir}/README.chromium").write(makeReadme(dependency))
            new File("${absoluteDepDir}/cipd.yaml").write(makeCipdYaml(dependency, repositoryPath))
            new File("${absoluteDepDir}/OWNERS").write(makeOwners())
            if (!dependency.licensePath?.trim()?.isEmpty()) {
                new File("${absoluteDepDir}/LICENSE").write(
                        new File("${normalisedRepoPath}/${dependency.licensePath}").text)
            } else if (!dependency.licenseUrl?.trim()?.isEmpty()) {
                downloadFile(dependency.licenseUrl, new File("${absoluteDepDir}/LICENSE"))
            }
        }

        // 3. Generate the root level build files
        updateBuildTargetDeclaration(graph, "${normalisedRepoPath}/BUILD.gn")
        updateDepsDeclaration(graph, repositoryPath, "${rootDirPath}/DEPS")
        dependencyDirectories.sort { path1, path2 -> return path1.compareTo(path2) }
        updateReadmeReferenceFile(dependencyDirectories,
                                  "${normalisedRepoPath}/additional_readme_paths.json")
    }

    private static void updateBuildTargetDeclaration(ChromiumDepGraph depGraph, String path) {
        File buildFile = new File(path)
        def sb = new StringBuilder()

        // Comparator to sort the dependency in alphabetical order, with the visible ones coming
        // before all the internal ones.
        def dependencyComparator = { dependency1, dependency2 ->
            def visibilityResult = Boolean.compare(dependency1.visible, dependency2.visible)
            if (visibilityResult != 0) return -visibilityResult

            return dependency1.id.compareTo(dependency2.id)
        }

        depGraph.dependencies.values().sort(dependencyComparator).each { dependency ->
            if (dependency.exclude || EXISTING_LIBS.get(dependency.id) != null) {
                return
            }
            def depsStr = ""
            if (!dependency.children.isEmpty()) {
                dependency.children.each { childDep ->
                    def dep = depGraph.dependencies[childDep];
                    if (dep.exclude) {
                        return
                    }
                    // Special case: If a child dependency is an existing lib, rather than skipping
                    // it, replace the child dependency with the existing lib.
                    def existingLib = EXISTING_LIBS.get(dep.id)
                    if (existingLib != null) {
                      depsStr += "\"${existingLib}\","
                    } else {
                      depsStr += "\":${dep.id}_java\","
                    }
                }
            }

            def libPath = "${DOWNLOAD_DIRECTORY_NAME}/${dependency.id}"
            sb.append(BUILD_GN_GEN_REMINDER)
            if (dependency.extension == 'jar') {
                sb.append("""\
                java_prebuilt("${dependency.id}_java") {
                  jar_path = "${libPath}/${dependency.fileName}"
                  output_name = "${dependency.id}"
                """.stripIndent())
                if (dependency.supportsAndroid) sb.append("  supports_android = true\n")
            } else if (dependency.extension == 'aar') {
                sb.append("""\
                android_aar_prebuilt("${dependency.id}_java") {
                  aar_path = "${libPath}/${dependency.fileName}"
                  info_path = "${libPath}/${dependency.id}.info"
                """.stripIndent())
            } else {
                throw new IllegalStateException("Dependency type should be JAR or AAR")
            }

            if (!dependency.visible) {
              sb.append("  # To remove visibility constraint, add this dependency to\n")
              sb.append("  # //tools/android/roll/android_deps/build.gradle.\n")
              sb.append("  visibility = [ \":*\" ]\n")
            }
            if (dependency.testOnly) sb.append("  testonly = true\n")
            if (!depsStr.empty) sb.append("  deps = [${depsStr}]\n")
            addSpecialTreatment(sb, dependency.id)

            sb.append("}\n\n")
        }

        def matcher = BUILD_GN_GEN_PATTERN.matcher(buildFile.getText())
        if (!matcher.find()) throw new IllegalStateException("BUILD.gn insertion point not found.")
        buildFile.write(matcher.replaceFirst(
                "${BUILD_GN_TOKEN_START}\n${sb.toString()}\n${BUILD_GN_TOKEN_END}"))
    }

    private static void addSpecialTreatment(StringBuilder sb, String dependencyId) {
        if (Pattern.matches(".*google.*play_services.*", dependencyId)) {
	    if (Pattern.matches(".*cast_framework.*", dependencyId)) {
                sb.append('  # Removing all resources from cast framework as they are unused bloat.\n')
                sb.append('  strip_resources = true\n')
	    } else {
                sb.append('  # Removing drawables from GMS .aars as they are unused bloat.\n')
                sb.append('  strip_drawables = true\n')
	    }
        }
        switch(dependencyId) {
            case 'com_android_support_support_compat':
            case 'com_android_support_support_media_compat':
                // Target has AIDL, but we don't support it yet: http://crbug.com/644439
                sb.append('  ignore_aidl = true\n')
                break
            case 'com_android_support_transition':
                // Not specified in the POM, compileOnly dependency not supposed to be used unless
                // the library is present: b/70887421
                sb.append('  deps += [":com_android_support_support_fragment_java"]\n')
                break
            case 'com_google_android_gms_play_services_basement':
                // Deprecated deps jar but still needed by play services basement.
                sb.append('  input_jars_paths=["\\$android_sdk/optional/org.apache.http.legacy.jar"]\n')
                break
            case 'com_google_ar_core':
                // Target .aar file contains .so libraries that need to be extracted,
                // and android_aar_prebuilt template will fail if it's not set explictly.
                sb.append('  extract_native_libraries = true\n')
                // InstallActivity class is downloaded as a part of DFM & we need to inject
                // a call to SplitCompat.install() into it.
                sb.append('  split_compat_class_names = [ "com/google/ar/core/InstallActivity" ]\n')
                break
            case 'androidx_test_rules':
                // Target needs Android SDK deps which exist in third_party/android_tools.
                sb.append("""\
                |  deps += [
                |    "//third_party/android_tools:android_test_base_java",
                |    "//third_party/android_tools:android_test_mock_java",
                |    "//third_party/android_tools:android_test_runner_java",
                |  ]
                |
                |""".stripMargin())
                break
        }
    }

    private static void updateDepsDeclaration(ChromiumDepGraph depGraph, String repoPath,
            String depsFilePath) {
        File depsFile = new File(depsFilePath)
        def sb = new StringBuilder()
        // Note: The string we're inserting is nested 1 level, hence the 2 leading spaces. Same
        // applies to the multiline package declaration string below.
        sb.append("  # Generated by //tools/android/roll/android_deps/fetch_all.py")

        // Comparator to sort the dependencies in alphabetical order.
        def dependencyComparator = { dependency1, dependency2 ->
            return dependency1.id.compareTo(dependency2.id)
        }

        depGraph.dependencies.values().sort(dependencyComparator).each { dependency ->
            if (dependency.exclude || EXISTING_LIBS.get(dependency.id) != null) {
                return
            }
            def depPath = "${DOWNLOAD_DIRECTORY_NAME}/${dependency.id}"
            sb.append("""\
            |
            |  'src/${repoPath}/${depPath}': {
            |      'packages': [
            |          {
            |              'package': 'chromium/${repoPath}/${depPath}',
            |              'version': 'version:${dependency.version}-${CIPD_SUFFIX}',
            |          },
            |      ],
            |      'condition': 'checkout_android',
            |      'dep_type': 'cipd',
            |  },
            |""".stripMargin())
        }

        def matcher = DEPS_GEN_PATTERN.matcher(depsFile.getText())
        if (!matcher.find()) throw new IllegalStateException("DEPS insertion point not found.")
        depsFile.write(matcher.replaceFirst("${DEPS_TOKEN_START}\n${sb}\n  ${DEPS_TOKEN_END}"))
    }

    private static void updateReadmeReferenceFile(List<String> directories, String readmePath) {
        File refFile = new File(readmePath)
        refFile.write(JsonOutput.prettyPrint(JsonOutput.toJson(directories)) + "\n")
    }

    private String normalisePath(String pathRelativeToChromiumRoot) {
        def pathToChromiumRoot = "../../../.." // Relative to build.gradle, the project root.
        return project.file("${pathToChromiumRoot}/${pathRelativeToChromiumRoot}").absolutePath
    }

    static String makeOwners() {
        // Make it easier to upgrade existing dependencies without full third_party review.
        return "file://third_party/android_deps/OWNERS"
    }

    static String makeReadme(ChromiumDepGraph.DependencyDescription dependency) {
        def licenseString
        // Replace license names with ones that are whitelisted, see third_party/PRESUBMIT.py
        switch (dependency.licenseName) {
            case "The Apache Software License, Version 2.0":
                licenseString = "Apache Version 2.0"
                break
            default:
                licenseString = dependency.licenseName
        }

        def securityCritical = dependency.supportsAndroid && !dependency.testOnly
        def licenseFile = securityCritical? "LICENSE" : "NOT_SHIPPED"

        return """\
        Name: ${dependency.displayName}
        Short Name: ${dependency.name}
        URL: ${dependency.url}
        Version: ${dependency.version}
        License: ${licenseString}
        License File: ${licenseFile}
        Security Critical: ${securityCritical? "yes" : "no"}
        ${dependency.licenseAndroidCompatible? "License Android Compatible: yes" : ""}
        Description:
        ${dependency.description}

        Local Modifications:
        No modifications.
        """.stripIndent()
    }

    static String makeCipdYaml(ChromiumDepGraph.DependencyDescription dependency, String repoPath) {
        def cipdVersion = "${dependency.version}-${CIPD_SUFFIX}"

        // NOTE: the fetch_all.py script relies on the format of this file!
        // See fetch_all.py:GetCipdPackageInfo().
        def str = """\
        # Copyright 2018 The Chromium Authors. All rights reserved.
        # Use of this source code is governed by a BSD-style license that can be
        # found in the LICENSE file.

        # To create CIPD package run the following command.
        # cipd create --pkg-def cipd.yaml -tag version:${cipdVersion}
        package: chromium/${repoPath}/${DOWNLOAD_DIRECTORY_NAME}/${dependency.id}
        description: "${dependency.displayName}"
        data:
        - file: ${dependency.fileName}
        """.stripIndent()

        return str
    }

    static String jsonDump(obj) {
        return JsonOutput.prettyPrint(JsonOutput.toJson(obj))
    }

    static void printDump(obj) {
        getLogger().warn(jsonDump(obj))
    }

    static void downloadFile(String sourceUrl, File destinationFile) {
        destinationFile.withOutputStream { out ->
            out << new URL(sourceUrl).openStream()
        }
    }

}
