# VC6ai

An AI agent that runs on Windows XP et al.

<img width="797" height="600" alt="image" src="https://github.com/user-attachments/assets/cd0d9eb3-d3fe-4c6a-83e5-b091bfd4597d" />

I built this over a weekend. It's about 700 lines of C, it compiles with Visual C++ 6 and it runs happily on a Windows Server 2003 box that has no business running anything from this decade. It talks to an LLM and it has exactly one tool.

## Why

The last couple of years gave us a plethora of agent frameworks, with a lot of them raising silly amount of VC money.
Most of that effort went into complex harnesses: special tools, strict schemas, MCP servers, etc. 
Supposedly all this scaffolding helps the model.

Today it feels these harnesses mostly get in the way. Every extra tool is more ambiguity for the model, more definitions stuffed into the context window and a bigger bill at the end.

Here's the thing though: modern models (released in 2026 and later) are already really good at using a shell.
Give one a command line and it will write files, (rip)grep through them, hit an API with curl, compile code, whatever. It doesn't need a custom tool for each of those. 

One tool: run a command.

## Rules I set for myself

A few (daft) self-imposed constraints:

- Keep it under 1000 lines of C89ish (VC6 is not known for standards conformity)
- One tool only: the model can run a `cmd.exe` command and nothing else.
- It has to run on a toaster i.e. my Windows Server 2003 R2 VM.
- No LLMs used for development, every line is handwritten, bringing joy.

## Getting it running

You'll need Visual C++ 6 to build it, libcurl (provided) and an OpenRouter API key.

Copy `vc6ai.sample.ini` to `vc6ai.ini` and set the OpenRouter API key.

```ini
[OpenRouter]
ApiKey=sk-or-v1-...
Model=deepseek/deepseek-v4-flash:nitro
Effort=xhigh
ZeroDataRetention=1
```

Any cheap current-gen model does the job (works extremely well with GLM-5.2!).

Build it in VC6, run the exe and it'll ask you for a prompt. Sometimes it wanders off, but it gets there often enough to be genuinely useful, which surprised me for something this small.

*Enjoy!*
