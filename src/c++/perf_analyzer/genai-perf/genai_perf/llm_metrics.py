#!/usr/bin/env python3

# Copyright 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import csv
import json
from itertools import pairwise
from typing import List

import numpy as np
import pandas as pd
from genai_perf.constants import DEFAULT_ARTIFACT_DIR
from genai_perf.llm_inputs.llm_inputs import OutputFormat
from genai_perf.tokenizer import Tokenizer
from genai_perf.utils import load_json, remove_sse_prefix
from rich.console import Console
from rich.table import Table

_OPENAI_CHAT_COMPLETIONS = OutputFormat.OPENAI_CHAT_COMPLETIONS
_OPENAI_COMPLETIONS = OutputFormat.OPENAI_COMPLETIONS


class Metrics:
    """A base class for all the metrics class that contains common metrics."""

    metric_labels = [
        "time_to_first_token",
        "inter_token_latency",
        "request_latency",
        "output_token_throughput",
        "output_token_throughput_per_request",
        "request_throughput",
        "num_output_token",
        "num_input_token",
    ]

    time_fields = [
        "inter_token_latency",
        "time_to_first_token",
        "request_latency",
    ]

    # TODO (TMA-1678): output_token_throughput_per_request is not on this list
    # since the current code treats all the throughput metrics to be displayed
    # outside of the statistics table.
    throughput_fields = [
        "request_throughput",
        "output_token_throughput",
    ]

    def __init__(
        self,
        request_throughputs: List[float] = [],
        request_latencies: List[int] = [],
    ) -> None:
        self.request_throughputs = request_throughputs
        self.request_latencies = request_latencies
        self._base_names = {
            "request_throughputs": "request_throughput",
            "request_latencies": "request_latency",
        }

    def __repr__(self):
        attr_strs = []
        for k, v in self.__dict__.items():
            if not k.startswith("_"):
                attr_strs.append(f"{k}={v}")
        return f"Metrics({','.join(attr_strs)})"

    @property
    def data(self) -> dict:
        """Returns all the metrics."""
        return {k: v for k, v in self.__dict__.items() if not k.startswith("_")}

    def get_base_name(self, metric_name: str) -> str:
        """Returns singular name of a given metric."""
        if metric_name in self._base_names:
            return self._base_names[metric_name]
        else:
            raise KeyError(f"No metric named '{metric_name}' exists.")


class LLMMetrics(Metrics):
    """A simple dataclass that holds core LLM performance metrics."""

    def __init__(
        self,
        request_throughputs: List[float] = [],
        request_latencies: List[int] = [],
        time_to_first_tokens: List[int] = [],
        inter_token_latencies: List[list[int]] = [[]],
        output_token_throughputs: List[float] = [],
        output_token_throughputs_per_request: List[int] = [],
        num_output_tokens: List[int] = [],
        num_input_tokens: List[int] = [],
    ) -> None:
        super().__init__(request_throughputs, request_latencies)
        self.time_to_first_tokens = time_to_first_tokens
        self.inter_token_latencies = inter_token_latencies
        self.output_token_throughputs = output_token_throughputs
        self.output_token_throughputs_per_request = output_token_throughputs_per_request
        self.num_output_tokens = num_output_tokens
        self.num_input_tokens = num_input_tokens

        # add base name mapping
        self._base_names["time_to_first_tokens"] = "time_to_first_token"
        self._base_names["inter_token_latencies"] = "inter_token_latency"
        self._base_names["output_token_throughputs"] = "output_token_throughput"
        self._base_names[
            "output_token_throughputs_per_request"
        ] = "output_token_throughput_per_request"
        self._base_names["num_output_tokens"] = "num_output_token"
        self._base_names["num_input_tokens"] = "num_input_token"


class Statistics:
    """A class that aggregates various statistics from given metrics class.

    The Statistics class goes through each metric in the metrics class and
    calculates several statistics such as:
      - average (arithmetic mean)
      - percentiles (p25, p50, p75, p90, p95, p99)
      - minimum & maximum
      - standard deviation
    The class will store each calculated statistics as part of its attribute.

    Example:

      >>> metrics = LLMMetrics(request_throughputs=[2, 4])
      >>> stats = Statistics(metrics)
      >>> print(stats.avg_request_throughput)  # output: 3
    """

    def __init__(self, metrics: Metrics):
        # iterate through Metrics to calculate statistics and set attributes
        self._metrics = metrics
        for attr, data in metrics.data.items():
            attr = metrics.get_base_name(attr)
            data = self._preprocess_data(data, attr)
            if data:
                self._calculate_mean(data, attr)
                self._calculate_percentiles(data, attr)
                self._calculate_minmax(data, attr)
                self._calculate_std(data, attr)

    def _preprocess_data(self, data: list, attr: str) -> list[int | float]:
        new_data = []
        if attr == "inter_token_latency":
            # flatten inter token latencies to 1D
            for d in data:
                new_data += d
        else:
            new_data = data
        return new_data

    def _calculate_mean(self, data: list[int | float], attr: str) -> None:
        avg = np.mean(data)
        setattr(self, "avg_" + attr, avg)

    def _calculate_percentiles(self, data: list[int | float], attr: str) -> None:
        p25, p50, p75 = np.percentile(data, [25, 50, 75])
        p90, p95, p99 = np.percentile(data, [90, 95, 99])
        setattr(self, "p25_" + attr, p25)
        setattr(self, "p50_" + attr, p50)
        setattr(self, "p75_" + attr, p75)
        setattr(self, "p90_" + attr, p90)
        setattr(self, "p95_" + attr, p95)
        setattr(self, "p99_" + attr, p99)

    def _calculate_minmax(self, data: list[int | float], attr: str) -> None:
        min, max = np.min(data), np.max(data)
        setattr(self, "min_" + attr, min)
        setattr(self, "max_" + attr, max)

    def _calculate_std(self, data: list[int | float], attr: str) -> None:
        std = np.std(data)
        setattr(self, "std_" + attr, std)

    def __repr__(self) -> str:
        attr_strs = []
        for k, v in self.__dict__.items():
            if not k.startswith("_"):
                attr_strs.append(f"{k}={v}")
        return f"Statistics({','.join(attr_strs)})"

    @property
    def data(self) -> dict:
        """Return all the aggregated statistics."""
        return {k: v for k, v in self.__dict__.items() if not k.startswith("_")}

    @property
    def metrics(self) -> Metrics:
        """Return the underlying metrics used to calculate the statistics."""
        return self._metrics

    def _is_throughput_field(self, field: str) -> bool:
        return field in Metrics.throughput_fields

    def _is_time_field(self, field: str) -> bool:
        return field in Metrics.time_fields

    def pretty_print(self) -> None:
        """Prints the statistics in a tabular format."""

        singular_metric_rows = []
        table = Table(title="LLM Metrics")

        table.add_column("Statistic", justify="right", style="cyan", no_wrap=True)
        stats = ["avg", "min", "max", "p99", "p90", "p75"]
        for stat in stats:
            table.add_column(stat, justify="right", style="green")

        for metric in Metrics.metric_labels:
            formatted_metric = metric.replace("_", " ").capitalize()

            # Throughput fields are printed after the table
            is_throughput_field = self._is_throughput_field(metric)
            if is_throughput_field:
                value = self.__dict__.get(f"{stats[0]}_{metric}", -1)
                formatted_metric += f" (per sec): {value:.2f}"
                singular_metric_rows.append(formatted_metric)
                continue

            # TODO (TMA-1712): need to decide if we need this metric. Remove
            # from statistics display for now.
            # TODO (TMA-1678): output_token_throughput_per_request is treated
            # separately since the current code treats all throughput metrics to
            # be displayed outside of the statistics table.
            if metric == "output_token_throughput_per_request":
                formatted_metric += f" (per sec)"
                continue

            is_time_field = self._is_time_field(metric)
            if is_time_field:
                formatted_metric += " (ns)"

            row_values = [formatted_metric]

            for stat in stats:
                value = self.__dict__.get(f"{stat}_{metric}", -1)
                row_values.append(f"{value:,.0f}")

            # Without streaming, there is no inter-token latency available, so do not print it.
            if metric == "inter_token_latency":
                if all(value == "-1" for value in row_values[1:]):
                    continue
            # Without streaming, TTFT and request latency are the same, so do not print TTFT.
            elif metric == "time_to_first_token":
                unique_values = False
                for stat in stats:
                    value_ttft = self.__dict__.get(f"{stat}_{metric}", -1)
                    value_req_latency = self.__dict__.get(f"{stat}_request_latency", -1)
                    if value_ttft != value_req_latency:
                        unique_values = True
                        break
                if not unique_values:
                    continue

            table.add_row(*row_values)

        console = Console()
        console.print(table)

        for row in singular_metric_rows:
            print(row)

    def export_to_csv(self, csv_filename: str) -> None:
        """Exports the statistics to a CSV file."""

        multiple_metric_header = [
            "Metric",
            "avg",
            "min",
            "max",
            "p99",
            "p95",
            "p90",
            "p75",
            "p50",
            "p25",
        ]

        single_metric_header = [
            "Metric",
            "Value",
        ]

        with open(csv_filename, mode="w", newline="") as csvfile:
            singular_metric_rows = []

            csv_writer = csv.writer(csvfile)
            csv_writer.writerow(multiple_metric_header)

            for metric in Metrics.metric_labels:
                formatted_metric = metric.replace("_", " ").title()

                is_throughput_field = self._is_throughput_field(metric)
                is_time_field = self._is_time_field(metric)

                if is_time_field:
                    formatted_metric += " (ns)"
                elif is_throughput_field:
                    formatted_metric += " (per sec)"
                # TODO (TMA-1712): need to decide if we need this metric. Do not
                # include in the csv for now.
                # TODO (TMA-1678): output_token_throughput_per_request is treated
                # separately since the current code treats all throughput metrics
                # to be displayed outside of the statistics table.
                elif metric == "output_token_throughput_per_request":
                    formatted_metric += " (per sec)"
                    continue

                row_values = [formatted_metric]

                if is_throughput_field:
                    value = self.__dict__.get(
                        f"{multiple_metric_header[1]}_{metric}", -1
                    )
                    row_values.append(f"{value:.2f}")
                    singular_metric_rows.append(row_values)
                    continue

                for stat in multiple_metric_header[1:]:
                    value = self.__dict__.get(f"{stat}_{metric}", -1)
                    row_values.append(f"{value:.0f}")

                # Without streaming, there is no inter-token latency available, so do not print it.
                if metric == "inter_token_latency":
                    if all(value == "-1" for value in row_values[1:]):
                        continue
                # Without streaming, TTFT and request latency are the same, so do not print TTFT.
                elif metric == "time_to_first_token":
                    unique_values = False
                    for stat in multiple_metric_header[1:]:
                        value_ttft = self.__dict__.get(f"{stat}_{metric}", -1)
                        value_req_latency = self.__dict__.get(
                            f"{stat}_request_latency", -1
                        )
                        if value_ttft != value_req_latency:
                            unique_values = True
                            break
                    if not unique_values:
                        continue

                csv_writer.writerow(row_values)

            csv_writer.writerow([])
            csv_writer.writerow(single_metric_header)
            for row in singular_metric_rows:
                csv_writer.writerow(row)

    def export_parquet(self, parquet_filename: str) -> None:
        max_length = -1
        col_index = 0
        filler_list = []
        df = pd.DataFrame()
        # Data frames require all columns of the same length
        # find the max length column
        for key, value in self._metrics.data.items():
            max_length = max(max_length, len(value))
        # Insert None for shorter columns to match longest column
        for key, value in self._metrics.data.items():
            if len(value) < max_length:
                diff = max_length - len(value)
                filler_list = [None] * diff
            df.insert(col_index, key, value + filler_list)
            diff = 0
            filler_list = []
            col_index = col_index + 1
        df.to_parquet(
            f"{DEFAULT_ARTIFACT_DIR}/data/{parquet_filename}.gzip", compression="gzip"
        )


class ProfileDataParser:
    """Base profile data parser class that reads the profile data JSON file to
    extract core metrics and calculate various performance statistics.
    """

    def __init__(self, filename: str) -> None:
        data = load_json(filename)
        self._parse_profile_data(data)

    def _parse_profile_data(self, data: dict) -> None:
        """Parse through the entire profile data to collect statistics."""
        self._profile_results = {}
        for experiment in data["experiments"]:
            infer_mode = experiment["experiment"]["mode"]
            load_level = experiment["experiment"]["value"]
            requests = experiment["requests"]

            metrics = self._parse_requests(requests)

            # aggregate and calculate statistics
            statistics = Statistics(metrics)
            self._profile_results[(infer_mode, str(load_level))] = statistics

    def _parse_requests(self, requests: dict) -> LLMMetrics:
        """Parse each request in profile data to extract core metrics."""
        raise NotImplementedError

    def get_statistics(self, infer_mode: str, load_level: str) -> Statistics:
        """Return profile statistics if it exists."""
        if (infer_mode, load_level) not in self._profile_results:
            raise KeyError(f"Profile with {infer_mode}={load_level} does not exist.")
        return self._profile_results[(infer_mode, load_level)]


class LLMProfileDataParser(ProfileDataParser):
    """A class that calculates and aggregates all the LLM performance statistics
    across the Perf Analyzer profile results.

    The LLMProfileDataParser class parses profile export JSON file, collects the
    core LLM performance metrics, and calculates summary statistics for each
    different Perf Analyzer runs/experiments.

    Example:

      >>> ... # run Perf Analyzer with concurrency level 10
      >>>
      >>> from transformers import AutoTokenizer
      >>>
      >>> tokenizer = AutoTokenizer.from_pretrained("gpt2")
      >>> pd = LLMProfileDataParser(
      >>>     filename="profile_export.json",
      >>>     service_kind="triton",
      >>>     tokenizer=tokenizer,
      >>> )
      >>> stats = pd.get_statistics(infer_mode="concurrency", level=10)
      >>>
      >>> print(stats)  # output: Statistics(avg_time_to_first_token=...)
      >>> stats.pretty_print()  # Output: time_to_first_token_s: ...
    """

    def __init__(
        self,
        filename: str,
        service_kind: str,
        output_format: OutputFormat,
        tokenizer: Tokenizer,
    ) -> None:
        self._tokenizer = tokenizer
        # Disable add_bos_token so that llama tokenizer does not add bos token
        # (aka. beginning-of-sentence) to the beginning of every response
        # outputs, increasing the token count by 1 for each output response.
        # Note: The type is being ignored here, because not all tokenizers have
        # an add_bos_token variable.
        self._tokenizer.add_bos_token = False  # type: ignore
        self._service_kind = service_kind
        self._output_format = output_format
        super().__init__(filename)

    def _parse_requests(self, requests: dict) -> LLMMetrics:
        """Parse each requests in profile export data to extract key metrics."""
        min_req_timestamp, max_res_timestamp = float("inf"), 0
        request_latencies = []
        time_to_first_tokens = []
        inter_token_latencies = []
        output_token_throughputs_per_request = []
        num_input_tokens = []
        num_generated_tokens = []
        for request in requests:
            req_timestamp = request["timestamp"]
            req_inputs = request["request_inputs"]
            res_timestamps = request["response_timestamps"]
            res_outputs = request["response_outputs"]

            self._preprocess_response(res_timestamps, res_outputs)

            # Skip requests with empty response. This happens sometimes when the
            # model returns a single response with empty string.
            if not res_timestamps:
                continue

            # track entire benchmark duration
            min_req_timestamp = min(min_req_timestamp, req_timestamp)
            max_res_timestamp = max(max_res_timestamp, res_timestamps[-1])

            # request latencies
            req_latency = res_timestamps[-1] - req_timestamp
            request_latencies.append(req_latency)  # nanosec
            req_latency = req_latency / 1e9  # sec

            # time to first token
            time_to_first_tokens.append(res_timestamps[0] - req_timestamp)

            # number of input tokens
            input_tokens = self._tokenize_request_inputs(req_inputs)
            num_input_tokens.append(len(input_tokens))

            # output token throughput per request
            output_tokens = self._tokenize_response_outputs(res_outputs)
            num_output_tokens = list(map(len, output_tokens))
            total_output_tokens = np.sum(num_output_tokens)
            output_token_throughputs_per_request.append(
                total_output_tokens / req_latency
            )
            num_generated_tokens.append(total_output_tokens)

            # inter token latency
            itl_per_request = []
            for (t1, _), (t2, n2) in pairwise(zip(res_timestamps, num_output_tokens)):
                # TMA-1676: handle empty first/last responses
                # if the latter response has zero token (e.g. empty string),
                # then set it default to one for the sake of inter token latency
                # calculation and to avoid divide by zero.
                num_token = 1 if n2 == 0 else n2
                itl_per_request.append(round((t2 - t1) / num_token))
            inter_token_latencies.append(itl_per_request)

        # request & output token throughput
        benchmark_duration = (max_res_timestamp - min_req_timestamp) / 1e9  # nanosec
        request_throughputs = [len(requests) / benchmark_duration]
        output_token_throughputs = [sum(num_generated_tokens) / benchmark_duration]

        return LLMMetrics(
            request_throughputs,
            request_latencies,
            time_to_first_tokens,
            inter_token_latencies,
            output_token_throughputs,
            output_token_throughputs_per_request,
            num_generated_tokens,
            num_input_tokens,
        )

    def _preprocess_response(
        self, res_timestamps: list[int], res_outputs: list[dict[str, str]]
    ) -> None:
        """Helper function to preprocess responses of a request."""
        if self._service_kind == "openai":
            # Remove responses without any content
            # These are only observed to happen at the start or end
            while res_outputs[0] and self._is_openai_empty_response(
                res_outputs[0]["response"]
            ):
                res_timestamps.pop(0)
                res_outputs.pop(0)

            while res_outputs[-1] and self._is_openai_empty_response(
                res_outputs[-1]["response"]
            ):
                res_timestamps.pop()
                res_outputs.pop()

    def _tokenize_request_inputs(self, req_inputs: dict) -> list[int]:
        """Deserialize the request input and return tokenized inputs."""
        if self._service_kind == "triton":
            return self._tokenize_triton_request_input(req_inputs)
        elif self._service_kind == "openai":
            return self._tokenize_openai_request_input(req_inputs)
        else:
            raise ValueError(f"Unknown service kind: '{self._service_kind}'.")

    def _tokenize_triton_request_input(self, req_inputs: dict) -> list[int]:
        """Tokenize the Triton request input texts."""
        encodings = self._tokenizer(req_inputs["text_input"])
        return encodings.data["input_ids"]

    def _tokenize_openai_request_input(self, req_inputs: dict) -> list[int]:
        """Tokenize the OpenAI request input texts."""
        payload = json.loads(req_inputs["payload"])
        if self._output_format == _OPENAI_CHAT_COMPLETIONS:
            input_text = payload["messages"][0]["content"]
        elif self._output_format == _OPENAI_COMPLETIONS:
            input_text = payload["prompt"][0]
        else:
            raise ValueError(
                "Failed to parse OpenAI request input in profile export file."
            )
        encodings = self._tokenizer(input_text)
        return encodings.data["input_ids"]

    def _tokenize_response_outputs(self, res_outputs: dict) -> list[list[int]]:
        """Deserialize the response output and return tokenized outputs."""
        if self._service_kind == "triton":
            return self._tokenize_triton_response_output(res_outputs)
        elif self._service_kind == "openai":
            return self._tokenize_openai_response_output(res_outputs)
        else:
            raise ValueError(f"Unknown service kind: '{self._service_kind}'.")

    def _tokenize_triton_response_output(self, res_outputs: dict) -> list[list[int]]:
        """Tokenize the Triton response output texts."""
        output_texts = []
        for output in res_outputs:
            output_texts.append(output["text_output"])
        return self._run_tokenizer(output_texts)

    def _tokenize_openai_response_output(self, res_outputs: dict) -> list[list[int]]:
        """Tokenize the OpenAI response output texts."""
        output_texts = []
        for output in res_outputs:
            text = self._extract_openai_text_output(output["response"])
            output_texts.append(text)
        return self._run_tokenizer(output_texts)

    def _run_tokenizer(self, output_texts: list[str]) -> list[list[int]]:
        # exclamation mark trick forces the llama tokenization to consistently
        # start each output with a specific token which allows us to safely skip
        # the first token of every tokenized output and get only the ones that
        # are returned by the model
        output_texts = ["!" + txt for txt in output_texts]
        encodings = self._tokenizer(output_texts)
        return [out[1:] for out in encodings.data["input_ids"]]

    def _extract_openai_text_output(self, response: str) -> str:
        """Extracts text/content of the OpenAI response object."""
        response = remove_sse_prefix(response)

        if response == "[DONE]":
            return ""

        data = json.loads(response)
        completions = data["choices"][0]

        text_output = ""
        if data["object"] == "text_completion":  # legacy
            text_output = completions.get("text", "")
        elif data["object"] == "chat.completion":  # non-streaming
            text_output = completions["message"]["content"]
        elif data["object"] == "chat.completion.chunk":  # streaming
            text_output = completions["delta"].get("content", "")
        else:
            obj_type = data["object"]
            raise ValueError(f"Unknown OpenAI response object type '{obj_type}'.")
        return text_output

    def _is_openai_empty_response(self, response: str) -> bool:
        """Returns true if the response is an openai response with no content (or empty content)"""
        text = self._extract_openai_text_output(response)
        if text:
            return False
        return True
