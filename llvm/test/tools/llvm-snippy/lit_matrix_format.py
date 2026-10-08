"""Lit test format that fans each Snippy test out over a matrix of simulator
models and deterministic seeds, then executes one (model, seed) configuration at
a time so a lit worker only ever buffers a single configuration's output in
memory (instead of the whole matrix's combined log).

A test selects its models with ``REQUIRES`` / ``UNSUPPORTED`` annotations:

* ``riscv-snippy-model-None``      -- run only with model-plugin None.
* ``riscv-snippy-model-<model>``   -- run only for the named simulator model.
* ``riscv-snippy-model-any``       -- run under every simulator model (but not
                                     the no-model ``None`` configuration).
* ``skip-snippy-model-mixin``      -- run for all seeds but not append any
                                     -model-plugin argument.
* ``riscv-snippy-model-callbacks`` -- capability flag: the test needs simulator
                                     callbacks.
* ``UNSUPPORTED: riscv-snippy-model-<model>`` -- drop that model from the test's
                                     ``any`` expansion.

A test with no model annotation runs on every simulator model *and* the
no-model (``None``) configuration. Each ``RUN:`` line whose command mentions
``llvm-snippy`` is expanded once per seed, injecting ``-model-plugin`` and
``-seed`` as appropriate. In ``single_run`` mode every test collapses to a single
(``model``, ``seed``) configuration, which is used for a fast smoke pass.
"""

import re
from dataclasses import dataclass
from typing import Optional

import lit.Test
import lit.TestRunner
import lit.formats

_MODEL_NONE = "riscv-snippy-model-None"
_MODEL_ANY = "riscv-snippy-model-any"
_MODEL_CALLBACKS = "riscv-snippy-model-callbacks"
_MODEL_MIXIN = "skip-snippy-model-mixin"
_MODEL_PREFIX = "riscv-snippy-model-"

NO_MODEL = "None"

KIND_ANY = "any"
KIND_NONE = "none"
KIND_SPECIFIC = "specific"
KIND_MIXIN = "mixin"

@dataclass(frozen=True)
class _ModelAnnotation:
    kind: str
    model: Optional[str] = None

@dataclass(frozen=True)
class _Config:
    model: str
    seed: int

class MatrixShTest(lit.formats.ShTest):
    def __init__(self, seeds_per_model=None, execute_external=True,
                 simulators_allowed=True, models_with_callbacks=None,
                 single_run=False):
        super().__init__(execute_external, force_execute_external=True)
        self.simulators_allowed = simulators_allowed
        # When True each test runs as a single (model, seed) config; otherwise
        # it runs every model/seed combination. Used for a fast smoke pass
        self.single_run = single_run
        self.seeds_per_model = seeds_per_model or {}
        # Concrete simulator models (everything but the ''/None aliases)
        self.models = [key for key in self.seeds_per_model
                       if key not in ('', NO_MODEL)]
        # Models that can run callback-requiring tests; when unspecified,
        # assume every model supports callbacks
        self.models_with_callbacks = (
            set(models_with_callbacks) if models_with_callbacks is not None
            else set(self.models))

    @classmethod
    def _classify(cls, feature):
        """Turn a ``REQUIRES`` token into a model annotation."""
        if feature == _MODEL_MIXIN:
            return _ModelAnnotation(KIND_MIXIN)
        if feature == _MODEL_NONE:
            return _ModelAnnotation(KIND_NONE)
        if feature == _MODEL_ANY:
            return _ModelAnnotation(KIND_ANY)
        if feature.startswith(_MODEL_PREFIX):
            return _ModelAnnotation(KIND_SPECIFIC, feature[len(_MODEL_PREFIX):])
        return None

    @classmethod
    def _is_exclusion(cls, feature):
        """True when *feature* (an ``UNSUPPORTED`` token) excludes a concrete
        simulator model from an ``any`` expansion."""
        return (feature.startswith(_MODEL_PREFIX)
                and feature != _MODEL_ANY
                and not feature.endswith(_MODEL_CALLBACKS))

    @staticmethod
    def _contains_snippy(cmd):
        return bool(re.search(r"\bllvm-snippy\b", cmd))

    def _partition_requires(self, requires):
        """Split REQUIRES tokens into ordinary features, the single model
        annotation, and a callbacks flag."""
        features = []
        annotation = None
        requires_callbacks = False
        for req in requires:
            if req == _MODEL_CALLBACKS:
                requires_callbacks = True
                continue
            ann = self._classify(req)
            if ann is None:
                features.append(req)
            elif annotation is None:
                annotation = ann
            else:
                raise ValueError(
                    f"conflicting model annotations: {annotation} and {ann}")
        return features, annotation, requires_callbacks

    def _partition_unsupported(self, unsupported):
        """Split UNSUPPORTED tokens into ordinary features and a set of models
        to exclude from the ``any`` expansion."""
        features = []
        excluded_models = set()
        for token in unsupported:
            if self._is_exclusion(token):
                excluded_models.add(token[len(_MODEL_PREFIX):])
            elif not self._classify(token):
                # Not a model annotation of any kind: keep it as an ordinary
                # unsupported feature (e.g. 'snippy-long-tests')
                features.append(token)
        return features, excluded_models

    def _seeds(self, model):
        return self.seeds_per_model.get(model, [])

    def _models_for(self, excluded_models, requires_callbacks):
        """Concrete models eligible for an expansion, honoring UNSUPPORTED
        exclusions and callback support."""
        return [
            model for model in self.models
            if model not in excluded_models
            and (not requires_callbacks
                 or model in self.models_with_callbacks)
        ]

    def _configs(self, model, seeds):
        """Configs for one model: every seed, or just the first in single-run
        mode (defaulting to seed 1 when the model has no seeds)."""
        if self.single_run:
            return [_Config(model, seeds[0] if seeds else 1)]
        return [_Config(model, seed) for seed in seeds]

    def _build_configs(self, annotation, requires_callbacks, excluded_models):
        """Build the (model, seed) configs the test must run.

        Returns a ``lit.Test.Result`` for an unsupported case, otherwise a list
        of :class:`_Config`.
        """
        if annotation is None:
            return self._default_configs(requires_callbacks, excluded_models)

        if annotation.kind == KIND_MIXIN:
            return self._configs('', self._seeds(''))
        if annotation.kind == KIND_NONE:
            return self._configs(NO_MODEL, self._seeds(NO_MODEL))
        if annotation.kind == KIND_SPECIFIC:
            if (requires_callbacks
                    and annotation.model not in self.models_with_callbacks):
                return lit.Test.Result(
                    lit.Test.UNSUPPORTED,
                    f"Model '{annotation.model}' does not support callbacks")
            return self._configs(annotation.model,
                                 self._seeds(annotation.model))

        # 'any': every simulator model, but never the no-model (None) config
        models = self._models_for(excluded_models, requires_callbacks)
        if self.single_run:
            return self._configs(models[0], self._seeds(models[0])) if models else []
        return [config for model in models
                for config in self._configs(model, self._seeds(model))]

    def _default_configs(self, requires_callbacks, excluded_models):
        """Configs for a test with no annotation: every simulator model plus the
        no-model (None) configuration, unless an UNSUPPORTED token or the
        callbacks requirement rules a config out."""
        configs = []
        if self.single_run:
            # One representative config: prefer a real simulator model
            models = self._models_for(excluded_models, requires_callbacks)
            if models:
                return self._configs(models[0], self._seeds(models[0]))
        else:
            for model in self._models_for(excluded_models, requires_callbacks):
                configs.extend(self._configs(model, self._seeds(model)))

        # The no-model config has no simulator, so it cannot provide callbacks
        if NO_MODEL not in excluded_models and not requires_callbacks:
            configs.extend(self._configs(NO_MODEL, self._seeds(NO_MODEL)))
        return configs

    @staticmethod
    def _check_features(requires, unsupported, available):
        """Return an UNSUPPORTED result if any *requires* feature is missing or
        any *unsupported* feature is available, otherwise ``None``."""
        missing = [req for req in requires if req not in available]
        if missing:
            return lit.Test.Result(
                lit.Test.UNSUPPORTED,
                "Test requires the following unavailable features: "
                + ", ".join(missing))
        forbidden = [feat for feat in unsupported if feat in available]
        if forbidden:
            return lit.Test.Result(
                lit.Test.UNSUPPORTED,
                "Test is unsupported with the following features: "
                + ", ".join(forbidden))
        return None

    @staticmethod
    def _parse_script(test):
        """Parse RUN/XFAIL/REQUIRES/UNSUPPORTED from the test file, populating
        the test's attributes and returning the RUN commands."""
        try:
            parsed = lit.TestRunner._parseKeywords(test.getSourcePath(), [], True)
        except ValueError as e:
            return lit.Test.Result(lit.Test.UNRESOLVED, str(e))
        script = parsed["RUN:"] or []
        test.xfails += parsed["XFAIL:"] or []
        test.requires += parsed["REQUIRES:"] or []
        test.unsupported += parsed["UNSUPPORTED:"] or []
        if parsed["ALLOW_RETRIES:"]:
            test.allowed_retries = parsed["ALLOW_RETRIES:"][0]
        return script

    @staticmethod
    def _expand_command(cmd, model, seed):
        """Inject ``-model-plugin`` (for a simulator) and ``-seed`` into a RUN
        command that invokes ``llvm-snippy``."""
        if model == '':
            replacement = f"llvm-snippy -seed {seed}"
        else:
            replacement = f"llvm-snippy -model-plugin {model} -seed {seed}"
        return re.sub(r"\bllvm-snippy\b", replacement, cmd)

    def _expand_commands(self, parsed_script, configs):
        """Expand each RUN command across every config.

        Returns a list of ``(config, commands)`` pairs so each (model, seed)
        configuration is kept as an independent unit.
        """
        groups = []
        for config in configs:
            commands = []
            for directive in parsed_script:
                cmd = getattr(directive, 'command', str(directive))
                if self._contains_snippy(cmd):
                    cmd = self._expand_command(cmd, config.model, config.seed)
                commands.append(cmd)
            groups.append((config, commands))
        return groups

    def execute(self, test, lit_config):
        if test.config.unsupported:
            return lit.Test.Result(lit.Test.UNSUPPORTED, "Test is unsupported")

        parsed_script = self._parse_script(test)
        if isinstance(parsed_script, lit.Test.Result):
            return parsed_script

        # Separate model selection and callbacks from ordinary feature checks
        requires, annotation, requires_callbacks = self._partition_requires(test.requires)
        unsupported, excluded_models = self._partition_unsupported(test.unsupported)

        configs = self._build_configs(
            annotation, requires_callbacks, excluded_models)
        if isinstance(configs, lit.Test.Result):
            return configs

        feature_result = self._check_features(
            requires, unsupported, set(test.config.available_features))
        if feature_result:
            return feature_result

        if lit_config.noExecute:
            return lit.Test.Result(lit.Test.PASS)

        # Expand commands, grouped by configuration, then run each one in turn
        tmp_dir, tmp_base = lit.TestRunner.getTempPaths(test)
        substitutions = lit.TestRunner.getDefaultSubstitutions(
            test, tmp_dir, tmp_base, normalize_slashes=self.execute_external)
        conditions = {feature: True
                      for feature in test.config.available_features}
        groups = self._expand_commands(parsed_script, configs)
        flat_script = [cmd for _, commands in groups for cmd in commands]
        if not flat_script:
            return lit.Test.Result(
                lit.Test.UNSUPPORTED,
                "Test requires sim X but snippy-seeds-num-X is set to 0")
        flat_script = lit.TestRunner.applySubstitutions(
            flat_script, substitutions, conditions,
            recursion_limit=test.config.recursiveExpansionLimit)

        # Every configuration expands the same number of commands (the parsed
        # directives), so the substituted flat list splits evenly per config
        per_config_cmds = len(parsed_script)
        per_config_scripts = [
            flat_script[i:i + per_config_cmds]
            for i in range(0, len(flat_script), per_config_cmds)
        ]
        return self._run_matrix(test, lit_config, per_config_scripts, tmp_base)

    def _run_matrix(self, test, lit_config, per_config_scripts, tmp_base):
        """Execute each configuration independently and aggregate the results."""
        code_rank = {
            lit.Test.PASS: 0,
            lit.Test.FLAKYPASS: 0,
            lit.Test.UNSUPPORTED: 1,
            lit.Test.UNRESOLVED: 2,
            lit.Test.FAIL: 3,
            lit.Test.TIMEOUT: 4,
        }
        results = [
            lit.TestRunner._runShTest(
                test, lit_config, self.execute_external, script, tmp_base)
            for script in per_config_scripts
        ]

        pass_codes = (lit.Test.PASS, lit.Test.FLAKYPASS, lit.Test.UNSUPPORTED)
        if all(result.code in pass_codes for result in results):
            return lit.Test.Result(lit.Test.PASS)

        fail_codes = (lit.Test.FAIL, lit.Test.TIMEOUT, lit.Test.UNRESOLVED)
        failed_output = "".join(
            result.output for result in results if result.code in fail_codes)
        worst = max(results, key=lambda r: code_rank.get(r.code, -1))
        return lit.Test.Result(
            worst.code, failed_output,
            attempts=worst.attempts, max_allowed_attempts=worst.max_allowed_attempts)
