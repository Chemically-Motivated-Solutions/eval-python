# omegajail

The omegaUp sandbox. Creates a userspace container with seccomp-bpf syscall
filtering in which the untrusted code is run.

```mermaid
sequenceDiagram
    participant C as Caller
    participant P as omegajail::Jail
    participant I as Sandboxed init
    participant J as Jailed process
    C->>P: let j = omegajail::Command::new(args).spawn()?;
    activate P;

    P->>I: clone(NEWUSER|NEWPID|NEWIPC|NEWUTS|NEWCGROUP)
    activate I;
    P->I: setup user namespace
    I->I: Setup all other namespaces
    I->>J: clone()
    activate J;
    I->>P: Setup CGroups Request
    P-->>I: Setup CGroups Response
    P-->>C: omegajail::Jail
    C->>P: j.wait()?;
    I->>J: Jailed process can start
    J->>J: Run jailed process
    alt process exits
        J-->>I: notification of exit
    else calls forbidden syscall
        J-->>I: seccomp-bpf notification
    end
    deactivate J;

    I-->>P: WaitidStatus
    deactivate I;
    P-->>C: JailResult
    deactivate P;
```

## Example

```ignore
let args = omegajail::Args{
  // ...
};
let result = omegajail::jail::Command::new(args).spawn()?.wait()?;
println!("{:?}", result);
```

## ATT&CK BERT Usage

ATT&CK BERT is a cybersecurity domain-specific language model based on sentence-transformers. ATT&CK BERT maps sentences representing attack actions to a semantically meaningful embedding vector. Embedding vectors of sentences with similar meanings have a high cosine similarity.

### Installation

Using this model becomes easy when you have sentence-transformers installed:

```sh
pip install -U sentence-transformers
```

### Example Code

```python
from sentence_transformers import SentenceTransformer
sentences = ["Attacker takes a screenshot", "Attacker captures the screen"]

model = SentenceTransformer('basel/ATTACK-BERT')
embeddings = model.encode(sentences)

from sklearn.metrics.pairwise import cosine_similarity
print(cosine_similarity([embeddings[0]], [embeddings[1]]))
```

To use ATT&CK BERT to map text to ATT&CK techniques, check our tool SMET: https://github.com/basel-a/SMET
