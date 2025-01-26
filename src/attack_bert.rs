use sentence_transformers::SentenceTransformer;
use sklearn::metrics::pairwise::cosine_similarity;

pub fn load_attack_bert_model() -> SentenceTransformer {
    SentenceTransformer::from_pretrained("basel/ATTACK-BERT")
}

pub fn encode_sentences(model: &SentenceTransformer, sentences: &[&str]) -> Vec<Vec<f32>> {
    model.encode(sentences)
}

pub fn calculate_cosine_similarity(embedding1: &[f32], embedding2: &[f32]) -> f32 {
    cosine_similarity(embedding1, embedding2)
}
